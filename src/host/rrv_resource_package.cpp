#include "rrv_resource_package.h"
#include "rrv_resource_package_common.h"

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
using detail::Sha256;
using detail::fail;
using detail::kVersion;
using detail::validate_relative;
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

std::string hash_fd(int fd) {
    if (lseek(fd, 0, SEEK_SET) < 0) fail("cannot seek package file"); Sha256 hash; std::array<unsigned char, 65536> data{};
    for (;;) { const ssize_t n = read(fd, data.data(), data.size()); if (n < 0) fail("cannot read package file"); if (!n) break; hash.update(data.data(), static_cast<size_t>(n)); }
    return hash.finish();
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
