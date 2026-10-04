#include "rrv_resource_package.h"

#if !((defined(__APPLE__) && defined(__aarch64__)) || (defined(__linux__) && defined(__x86_64__)))
#error "ADR-0006 resource packages are native macOS ARM64 or Linux x86-64 only"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <climits>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rrv::resource_package {
namespace {
constexpr std::string_view kVersion = "rrv-resource-package-v1";

[[noreturn]] void fail(const char* message) { throw std::runtime_error(std::string("resource package: ") + message); }
bool regular(const struct stat& info) { return S_ISREG(info.st_mode) && info.st_nlink == 1; }
bool directory(const struct stat& info) { return S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode); }
class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) { if (fd < 0) fail("cannot open package node"); }
    ~Fd() { if (fd_ >= 0) ::close(fd_); }
    Fd(const Fd&) = delete; Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept { if (this != &other) { if (fd_ >= 0) ::close(fd_); fd_ = other.release(); } return *this; }
    int get() const { return fd_; }
    int release() noexcept { const int value = fd_; fd_ = -1; return value; }
private: int fd_;
};

class Sha256 {
public:
    void update(const unsigned char* data, size_t count) {
        bits_ += static_cast<uint64_t>(count) * 8;
        while (count--) { block_[used_++] = *data++; if (used_ == 64) { transform(); used_ = 0; } }
    }
    std::string finish() {
        const uint64_t length = bits_; const unsigned char one = 0x80, zero = 0;
        update(&one, 1); while (used_ != 56) update(&zero, 1);
        std::array<unsigned char, 8> bytes{};
        for (int i = 0; i != 8; ++i) bytes[7 - i] = static_cast<unsigned char>(length >> (8 * i));
        update(bytes.data(), bytes.size());
        constexpr char hex[] = "0123456789abcdef"; std::string out; out.reserve(64);
        for (uint32_t word : state_) for (int shift = 28; shift >= 0; shift -= 4) out.push_back(hex[(word >> shift) & 15]);
        return out;
    }
private:
    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    void transform() {
        static constexpr uint32_t k[] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::array<uint32_t, 64> w{};
        for (size_t i=0;i<16;++i) w[i]=(uint32_t(block_[4*i])<<24)|(uint32_t(block_[4*i+1])<<16)|(uint32_t(block_[4*i+2])<<8)|block_[4*i+3];
        for (size_t i=16;i<64;++i) { const auto s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3); const auto s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
        auto a=state_[0],b=state_[1],c=state_[2],d=state_[3],e=state_[4],f=state_[5],g=state_[6],h=state_[7];
        for(size_t i=0;i<64;++i){const auto t1=h+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^((~e)&g))+k[i]+w[i];const auto t2=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        state_[0]+=a;state_[1]+=b;state_[2]+=c;state_[3]+=d;state_[4]+=e;state_[5]+=f;state_[6]+=g;state_[7]+=h;
    }
    std::array<unsigned char,64> block_{}; size_t used_=0; uint64_t bits_=0;
    std::array<uint32_t,8> state_{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
};

std::string hash_fd(int fd) {
    if (lseek(fd, 0, SEEK_SET) < 0) fail("cannot seek package file"); Sha256 hash; std::array<unsigned char, 65536> data{};
    for (;;) { const ssize_t n = read(fd, data.data(), data.size()); if (n < 0) fail("cannot read package file"); if (!n) break; hash.update(data.data(), static_cast<size_t>(n)); }
    return hash.finish();
}
void validate_relative(std::string_view value) {
    if (value.empty() || value.front() == '/' || value.back() == '/') fail("invalid package relative path");
    size_t begin = 0;
    while (begin < value.size()) { size_t end = value.find('/', begin); auto part = value.substr(begin, end == std::string_view::npos ? value.size()-begin : end-begin); if (part.empty() || part=="." || part=="..") fail("invalid package relative path"); for(char ch:part) if (!((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'||ch=='.')) fail("invalid package relative path"); if(end==std::string_view::npos) break; begin=end+1; }
}
int open_relative(int root, std::string_view path, bool as_directory=false) {
    validate_relative(path); int fd=dup(root); if(fd<0) fail("cannot duplicate package descriptor");
    try { size_t begin=0; while(begin<path.size()){ const size_t end=path.find('/',begin); const auto part=path.substr(begin,end==std::string_view::npos?path.size()-begin:end-begin); const bool final=end==std::string_view::npos; const int next=openat(fd,std::string(part).c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|((as_directory||!final)?O_DIRECTORY:0)); if(next<0) fail("cannot open package node"); close(fd);fd=next;if(final)break;begin=end+1;} return fd; } catch(...) { close(fd); throw; }
}
int open_absolute_directory(const std::filesystem::path& path) {
    if (!path.is_absolute()) fail("fixed root is not absolute");
    Fd fd(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    for (const auto& component : path.relative_path()) {
        const auto name = component.string();
        Fd next(openat(fd.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        fd = std::move(next);
    }
    struct stat info{}; if (fstat(fd.get(), &info) != 0 || !directory(info)) fail("fixed root is unsafe");
    return fd.release();
}
std::filesystem::path executable_path() {
#if defined(__APPLE__)
    uint32_t size=0; _NSGetExecutablePath(nullptr,&size); std::vector<char> raw(size);
    if (_NSGetExecutablePath(raw.data(),&size) != 0) fail("cannot obtain actual process image");
#else
    // Linux (Gate 5): the kernel's link to the actual process image.
    std::vector<char> raw(PATH_MAX + 1, '\0');
    const ssize_t length = readlink("/proc/self/exe", raw.data(), PATH_MAX);
    if (length <= 0 || length >= PATH_MAX) fail("cannot obtain actual process image");
    raw[static_cast<size_t>(length)] = '\0';
#endif
    std::array<char, PATH_MAX> resolved{}; if (!realpath(raw.data(),resolved.data())) fail("cannot resolve actual process image");
    return std::filesystem::path(resolved.data());
}
bool bound(const Binding& binding, std::string_view path, EntryType type) {
    for(size_t i=0;i<binding.inventory_count;++i) if(binding.inventory[i].relative==path && binding.inventory[i].type==type) return true;
    return false;
}
void enumerate(int fd, std::string prefix, std::unordered_map<std::string, EntryType>& actual) {
    int duplicate=dup(fd); if(duplicate<0) fail("cannot duplicate package directory"); DIR* stream=fdopendir(duplicate); if(!stream){close(duplicate);fail("cannot enumerate package directory");}
    try { while(dirent* entry=readdir(stream)){std::string name=entry->d_name;if(name=="."||name=="..")continue;struct stat info{};if(fstatat(fd,name.c_str(),&info,AT_SYMLINK_NOFOLLOW)!=0||S_ISLNK(info.st_mode))fail("package contains an unsafe node");std::string path=prefix.empty()?name:prefix+"/"+name;if(directory(info)){actual.emplace(path,EntryType::directory);int child=openat(fd,name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(child<0)fail("cannot open package directory");try{enumerate(child,path,actual);}catch(...){close(child);throw;}close(child);}else if(regular(info))actual.emplace(path,EntryType::file);else fail("package contains an unsafe node");} } catch(...) { closedir(stream); throw; }
    closedir(stream);
}
void verify(int fd, const Binding& binding) {
    if(binding.version!=kVersion || binding.composition.size()!=64 || !bound(binding,binding.bridge_relative,EntryType::file)) fail("invalid typed package binding");
    std::string expected_identity=std::string(kVersion)+"\n"+std::string(binding.composition)+"\n"; int identity=open_relative(fd,"identity"); std::string observed;
    try { std::array<char,128> bytes{}; ssize_t n=read(identity,bytes.data(),bytes.size()); if(n<0)fail("cannot read package identity"); observed.assign(bytes.data(),static_cast<size_t>(n)); if(read(identity,bytes.data(),1)!=0)fail("package identity is oversized"); } catch(...) {close(identity);throw;} close(identity); if(observed!=expected_identity)fail("package identity mismatch");
    std::unordered_map<std::string,EntryType> actual; enumerate(fd,"",actual); auto it=actual.find("identity"); if(it==actual.end()||it->second!=EntryType::file)fail("package identity is absent"); actual.erase(it);
    if(actual.size()!=binding.inventory_count)fail("package inventory has unexpected entries");
    for(size_t i=0;i<binding.inventory_count;++i){const auto& entry=binding.inventory[i];validate_relative(entry.relative);auto found=actual.find(std::string(entry.relative));if(found==actual.end()||found->second!=entry.type)fail("package inventory mismatch");if(entry.type==EntryType::file){int node=open_relative(fd,entry.relative);try{struct stat info{};if(fstat(node,&info)!=0||!regular(info)||uint64_t(info.st_size)!=entry.size||hash_fd(node)!=entry.sha256)fail("package file digest mismatch");}catch(...){close(node);throw;}close(node);}}
}
std::filesystem::path canonical_file(const std::filesystem::path& path) { std::array<char,PATH_MAX> buffer{}; if(!realpath(path.c_str(),buffer.data()))fail("cannot canonicalize package bridge"); return buffer.data(); }
} // namespace

ReaderLease::ReaderLease(int lock_fd) : fd_(lock_fd) {
    if(fd_<0) fail("cannot open stable package lock"); struct stat info{}; if(fstat(fd_,&info)!=0||!regular(info)||flock(fd_,LOCK_SH)!=0){close(fd_);fd_=-1;fail("cannot acquire package reader lease");}
}
ReaderLease::~ReaderLease(){if(fd_>=0)close(fd_);} ReaderLease::ReaderLease(ReaderLease&& other) noexcept:fd_(other.fd_){other.fd_=-1;} ReaderLease& ReaderLease::operator=(ReaderLease&& other) noexcept{if(this!=&other){if(fd_>=0)close(fd_);fd_=other.fd_;other.fd_=-1;}return *this;}

Package::Package(Binding binding,std::filesystem::path root,int fd,ReaderLease lease):binding_(binding),root_(std::move(root)),package_fd_(fd),lease_(std::move(lease)){}
Package::~Package(){close();} Package::Package(Package&& other) noexcept:binding_(other.binding_),root_(std::move(other.root_)),package_fd_(other.package_fd_),image_(other.image_),loaded_image_(std::move(other.loaded_image_)),lease_(std::move(other.lease_)){other.package_fd_=-1;other.image_=nullptr;} Package& Package::operator=(Package&& other) noexcept{if(this!=&other){close();binding_=other.binding_;root_=std::move(other.root_);package_fd_=other.package_fd_;image_=other.image_;loaded_image_=std::move(other.loaded_image_);lease_=std::move(other.lease_);other.package_fd_=-1;other.image_=nullptr;}return *this;}
void Package::close() noexcept { if(image_){dlclose(image_);image_=nullptr;} loaded_image_.clear(); if(package_fd_>=0){::close(package_fd_);package_fd_=-1;} }

Package Package::Open(const Binding& binding) {
    const auto image=executable_path(); const auto bin=image.parent_path(); std::filesystem::path build;
    // Fixed layouts only: B/bin/<image> (build trees, runtime packages) or, for
    // Fukami.app (Gate-9 APP1), X.app/Contents/MacOS/<image> with the package
    // at X.app/Contents/Resources/game. The Linux app (Gate 5) is
    // Fukami/bin/Fukami with the package at Fukami/share/game.
#if defined(__linux__)
    if(bin.filename()=="bin"&&image.filename()=="Fukami") build=bin.parent_path()/"share"/"game";
    else
#endif
    if(bin.filename()=="bin") build=bin.parent_path();
    else if(bin.filename()=="MacOS"&&bin.parent_path().filename()=="Contents"&&bin.parent_path().parent_path().extension()==".app") build=bin.parent_path()/"Resources"/"game";
    else fail("process image is not in fixed B/bin layout");
    Fd build_fd(open_absolute_directory(build));
    Fd lock_fd(open_relative(build_fd.get(), ".rrv-resource-package.lock")); ReaderLease lease(lock_fd.release());
    Fd package_fd(open_relative(build_fd.get(), ".rrv-resource-packages/current", true));
    const auto root=build/".rrv-resource-packages/current"; verify(package_fd.get(),binding);
    return Package(binding,root,package_fd.release(),std::move(lease));
}
std::filesystem::path Package::bridge_path() const { return resource_path(binding_.bridge_relative); }
std::filesystem::path Package::resource_path(std::string_view relative) const { if(!bound(binding_,relative,EntryType::file)&&!bound(binding_,relative,EntryType::directory))fail("unbound resource lookup"); validate_relative(relative); return root_/std::string(relative); }
void Package::open_bridge(){if(image_)return;const auto bridge=bridge_path();image_=dlopen(bridge.c_str(),RTLD_NOW|RTLD_LOCAL);if(!image_)fail("fixed bridge load failed");}
void* Package::optional_symbol(std::string_view name){if(name.empty()||name.find('\0')!=std::string_view::npos)fail("invalid bridge symbol");open_bridge();dlerror();void* value=dlsym(image_,std::string(name).c_str());if(const char* error=dlerror()){(void)error;return nullptr;}if(!value)fail("bridge symbol lookup failed");Dl_info loaded{};if(!dladdr(value,&loaded)||!loaded.dli_fname)fail("loaded bridge image identity mismatch"); const auto observed=canonical_file(loaded.dli_fname); const auto expected=canonical_file(bridge_path()); struct stat expected_stat{}, observed_stat{}; Fd expected_fd(open_relative(package_fd_,binding_.bridge_relative)); if(fstat(expected_fd.get(),&expected_stat)!=0||stat(observed.c_str(),&observed_stat)!=0||expected_stat.st_dev!=observed_stat.st_dev||expected_stat.st_ino!=observed_stat.st_ino||observed!=expected)fail("loaded bridge image identity mismatch");loaded_image_=observed;return value;}
void* Package::symbol(std::string_view name){if(void* value=optional_symbol(name))return value;fail("bridge symbol is absent");}
std::filesystem::path Package::loaded_image_path() const { if (loaded_image_.empty()) fail("bridge image has not been identity-checked"); return loaded_image_; }
} // namespace rrv::resource_package
