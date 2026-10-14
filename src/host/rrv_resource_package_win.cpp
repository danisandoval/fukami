// Windows implementation of the ADR-0006 package reader (rrv_resource_package.h). The macOS and Linux reader walks the
// package with descriptors and openat/O_NOFOLLOW; Windows has no openat, so every node is opened by path with
// FILE_FLAG_OPEN_REPARSE_POINT and judged from its own handle: a reparse point (symbolic link, junction, cloud
// placeholder), a node of the wrong kind or a file with more than one link is refused. The package root and the stable
// lock are held open without FILE_SHARE_DELETE, so the tree cannot be renamed or replaced under a running reader,
// and the final path of each opened handle must be exactly the expected path below the package root, which excludes a
// reparse point anywhere in the chain.
#include "rrv_resource_package.h"
#include "rrv_resource_package_common.h"

#if !defined(_WIN32)
#error "rrv_resource_package_win.cpp is the Windows implementation"
#endif

#include <algorithm>
#include <array>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace rrv::resource_package {
namespace {
using detail::Sha256;
using detail::fail;
using detail::kVersion;
using detail::validate_relative;

HANDLE raw(NativeHandle handle) { return static_cast<HANDLE>(handle); }
bool valid(NativeHandle handle) { return handle != kInvalidHandle && handle != nullptr; }
void close_handle(NativeHandle& handle) noexcept { if (valid(handle)) CloseHandle(raw(handle)); handle = kInvalidHandle; }

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) fail("package name is not valid UTF-8");
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::string narrow(const wchar_t* text) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string out(static_cast<size_t>(count - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), count, nullptr, nullptr);
    return out;
}

// An owned handle that must be valid.
class Handle {
public:
    explicit Handle(NativeHandle handle = kInvalidHandle) : handle_(handle) { if (!valid(handle)) fail("cannot open package node"); }
    ~Handle() { close_handle(handle_); }
    Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : handle_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) { close_handle(handle_); handle_ = other.release(); } return *this; }
    NativeHandle get() const { return handle_; }
    NativeHandle release() noexcept { const NativeHandle value = handle_; handle_ = kInvalidHandle; return value; }
private:
    NativeHandle handle_;
};

BY_HANDLE_FILE_INFORMATION information(NativeHandle handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(raw(handle), &info)) fail("cannot inspect package node");
    return info;
}
bool is_directory(const BY_HANDLE_FILE_INFORMATION& info) { return (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
bool is_reparse(DWORD attributes) { return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0; }
bool regular(const BY_HANDLE_FILE_INFORMATION& info) {
    return !is_directory(info) && !is_reparse(info.dwFileAttributes) && info.nNumberOfLinks == 1;
}
bool plain_directory(const BY_HANDLE_FILE_INFORMATION& info) { return is_directory(info) && !is_reparse(info.dwFileAttributes); }
uint64_t file_size(const BY_HANDLE_FILE_INFORMATION& info) { return (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow; }

// The canonical DOS path of an open handle ("\\?\C:\..."), as the file system reports it.
std::wstring final_path(NativeHandle handle) {
    std::wstring out(512, L'\0');
    for (;;) {
        const DWORD length = GetFinalPathNameByHandleW(raw(handle), out.data(), static_cast<DWORD>(out.size()), VOLUME_NAME_DOS);
        if (length == 0) fail("cannot resolve package node path");
        if (length < out.size()) { out.resize(length); return out; }
        out.resize(length + 1);
    }
}
bool same_path(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// Opens one node by path without following a reparse point; shares read and write but not delete, so the node cannot be
// renamed or removed while held. The result is checked against `want_directory`.
Handle open_node(const std::wstring& path, bool want_directory) {
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | (want_directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
    Handle handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, flags, nullptr));
    const auto info = information(handle.get());
    if (want_directory ? !plain_directory(info) : !regular(info)) fail("cannot open package node");
    return handle;
}

// B-relative open: every component is opened in turn (so each is checked), and the last handle's own final path must be
// exactly root + relative.
Handle open_relative(NativeHandle root, std::string_view path, bool as_directory = false) {
    validate_relative(path);
    const std::wstring base = final_path(root);
    std::wstring current = base;
    std::vector<Handle> held;
    size_t begin = 0;
    for (;;) {
        const size_t end = path.find('/', begin);
        const bool last = end == std::string_view::npos;
        current += L'\\';
        current += widen(path.substr(begin, last ? path.size() - begin : end - begin));
        held.push_back(open_node(current, as_directory || !last));
        if (last) break;
        begin = end + 1;
    }
    if (!same_path(final_path(held.back().get()), current)) fail("package node resolves outside the package");
    return std::move(held.back());
}

std::string hash_handle(NativeHandle handle) {
    if (SetFilePointer(raw(handle), 0, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) fail("cannot seek package file");
    Sha256 hash; std::array<unsigned char, 65536> data{};
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(raw(handle), data.data(), static_cast<DWORD>(data.size()), &got, nullptr)) fail("cannot read package file");
        if (got == 0) break;
        hash.update(data.data(), got);
    }
    return hash.finish();
}

std::filesystem::path executable_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) fail("cannot obtain actual process image");
        if (length < buffer.size()) { buffer.resize(length); break; }
        buffer.resize(buffer.size() * 2);
    }
    std::error_code error;
    auto resolved = std::filesystem::canonical(std::filesystem::path(buffer), error);
    if (error) fail("cannot resolve actual process image");
    return resolved;
}

bool bound(const Binding& binding, std::string_view path, EntryType type) {
    for (size_t i = 0; i < binding.inventory_count; ++i)
        if (binding.inventory[i].relative == path && binding.inventory[i].type == type) return true;
    return false;
}

void enumerate(const std::wstring& directory, const std::string& prefix, std::unordered_map<std::string, EntryType>& actual) {
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileExW((directory + L"\\*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) fail("cannot enumerate package directory");
    try {
        do {
            const std::wstring name = entry.cFileName;
            if (name == L"." || name == L"..") continue;
            if (is_reparse(entry.dwFileAttributes)) fail("package contains an unsafe node");
            const std::string text = narrow(entry.cFileName);
            const std::string path = prefix.empty() ? text : prefix + "/" + text;
            const std::wstring child = directory + L"\\" + name;
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                Handle node = open_node(child, true);  // a plain directory, held while it is listed
                actual.emplace(path, EntryType::directory);
                enumerate(child, path, actual);
            } else {
                Handle node = open_node(child, false);  // a single-link regular file
                actual.emplace(path, EntryType::file);
            }
        } while (FindNextFileW(find, &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES) fail("cannot enumerate package directory");
    } catch (...) { FindClose(find); throw; }
    FindClose(find);
}

void verify(NativeHandle root, const Binding& binding) {
    if (binding.version != kVersion || binding.composition.size() != 64 || !bound(binding, binding.bridge_relative, EntryType::file))
        fail("invalid typed package binding");
    const std::string expected_identity = std::string(kVersion) + "\n" + std::string(binding.composition) + "\n";
    std::string observed;
    {
        Handle identity = open_relative(root, "identity");
        std::array<char, 128> bytes{}; DWORD got = 0;
        if (!ReadFile(raw(identity.get()), bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr)) fail("cannot read package identity");
        observed.assign(bytes.data(), got);
        char extra = 0; DWORD more = 0;
        if (!ReadFile(raw(identity.get()), &extra, 1, &more, nullptr) || more != 0) fail("package identity is oversized");
    }
    if (observed != expected_identity) fail("package identity mismatch");
    std::unordered_map<std::string, EntryType> actual;
    enumerate(final_path(root), "", actual);
    auto it = actual.find("identity");
    if (it == actual.end() || it->second != EntryType::file) fail("package identity is absent");
    actual.erase(it);
    if (actual.size() != binding.inventory_count) fail("package inventory has unexpected entries");
    for (size_t i = 0; i < binding.inventory_count; ++i) {
        const auto& entry = binding.inventory[i];
        validate_relative(entry.relative);
        auto found = actual.find(std::string(entry.relative));
        if (found == actual.end() || found->second != entry.type) fail("package inventory mismatch");
        if (entry.type == EntryType::file) {
            Handle node = open_relative(root, entry.relative);
            if (file_size(information(node.get())) != entry.size || hash_handle(node.get()) != entry.sha256) fail("package file digest mismatch");
        }
    }
}

std::filesystem::path canonical_file(const std::filesystem::path& path) {
    std::error_code error;
    auto resolved = std::filesystem::canonical(path, error);
    if (error) fail("cannot canonicalize package bridge");
    return resolved;
}
}  // namespace

ReaderLease::ReaderLease(NativeHandle lock_handle) : fd_(lock_handle) {
    if (!valid(fd_)) fail("cannot open stable package lock");
    const auto info = information(fd_);
    OVERLAPPED overlapped{};
    // A shared lock on the first byte, held for the lease's lifetime; the producer takes the same byte exclusively.
    if (!regular(info) || !LockFileEx(raw(fd_), 0, 0, 1, 0, &overlapped)) { close_handle(fd_); fail("cannot acquire package reader lease"); }
}
ReaderLease::~ReaderLease() { close_handle(fd_); }
ReaderLease::ReaderLease(ReaderLease&& other) noexcept : fd_(other.fd_) { other.fd_ = kInvalidHandle; }
ReaderLease& ReaderLease::operator=(ReaderLease&& other) noexcept {
    if (this != &other) { close_handle(fd_); fd_ = other.fd_; other.fd_ = kInvalidHandle; }
    return *this;
}

Package::Package(Binding binding, std::filesystem::path root, NativeHandle fd, ReaderLease lease)
    : binding_(binding), root_(std::move(root)), package_fd_(fd), lease_(std::move(lease)) {}
Package::~Package() { close(); }
Package::Package(Package&& other) noexcept
    : binding_(other.binding_), root_(std::move(other.root_)), package_fd_(other.package_fd_), image_(other.image_),
      loaded_image_(std::move(other.loaded_image_)), lease_(std::move(other.lease_)) {
    other.package_fd_ = kInvalidHandle; other.image_ = nullptr;
}
Package& Package::operator=(Package&& other) noexcept {
    if (this != &other) {
        close(); binding_ = other.binding_; root_ = std::move(other.root_); package_fd_ = other.package_fd_; image_ = other.image_;
        loaded_image_ = std::move(other.loaded_image_); lease_ = std::move(other.lease_);
        other.package_fd_ = kInvalidHandle; other.image_ = nullptr;
    }
    return *this;
}
void Package::close() noexcept {
    if (image_) { FreeLibrary(static_cast<HMODULE>(image_)); image_ = nullptr; }
    loaded_image_.clear();
    close_handle(package_fd_);
}

Package Package::Open(const Binding& binding) {
    const auto image = executable_path();
    const auto bin = image.parent_path();
    std::filesystem::path build;
    // Fixed layouts only: B/bin/<image> (build trees, runtime packages) or the Windows app, Fukami/Fukami.exe with the
    // package at Fukami/resources (the layout scripts/package_fukami_windows.py writes).
    if (_wcsicmp(bin.filename().c_str(), L"bin") == 0) build = bin.parent_path();
    else if (_wcsicmp(image.filename().c_str(), L"Fukami.exe") == 0) build = bin / L"resources";
    else fail("process image is not in fixed B/bin layout");
    Handle build_handle(open_node(build.wstring(), true));
    Handle lock_handle(open_relative(build_handle.get(), ".rrv-resource-package.lock"));
    ReaderLease lease(lock_handle.release());
    Handle package_handle(open_relative(build_handle.get(), ".rrv-resource-packages/current", true));
    const auto root = build / L".rrv-resource-packages" / L"current";
    verify(package_handle.get(), binding);
    return Package(binding, root, package_handle.release(), std::move(lease));
}

std::filesystem::path Package::bridge_path() const { return resource_path(binding_.bridge_relative); }
std::filesystem::path Package::resource_path(std::string_view relative) const {
    if (!bound(binding_, relative, EntryType::file) && !bound(binding_, relative, EntryType::directory)) fail("unbound resource lookup");
    validate_relative(relative);
    return root_ / std::filesystem::path(widen(relative));
}

void Package::open_bridge() {
    if (image_) return;
    const auto bridge = bridge_path();
    // The bridge's own directory is searched first for the DLLs it imports.
    image_ = LoadLibraryExW(bridge.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!image_) fail("fixed bridge load failed");
}

void* Package::optional_symbol(std::string_view name) {
    if (name.empty() || name.find('\0') != std::string_view::npos) fail("invalid bridge symbol");
    open_bridge();
    SetLastError(0);
    void* value = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(image_), std::string(name).c_str()));
    if (!value) {
        if (GetLastError() == ERROR_PROC_NOT_FOUND) return nullptr;  // the bridge genuinely does not export it
        fail("bridge symbol lookup failed");
    }
    // The module that owns the address, its file, and that file's identity must all be the verified bridge.
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(value), &owner) || owner != static_cast<HMODULE>(image_))
        fail("loaded bridge image identity mismatch");
    std::wstring file(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(owner, file.data(), static_cast<DWORD>(file.size()));
        if (length == 0) fail("loaded bridge image identity mismatch");
        if (length < file.size()) { file.resize(length); break; }
        file.resize(file.size() * 2);
    }
    const auto observed = canonical_file(file);
    const auto expected = canonical_file(bridge_path());
    Handle expected_handle(open_relative(package_fd_, binding_.bridge_relative));
    Handle observed_handle(CreateFileW(observed.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, 0, nullptr));
    const auto expected_info = information(expected_handle.get());
    const auto observed_info = information(observed_handle.get());
    if (expected_info.dwVolumeSerialNumber != observed_info.dwVolumeSerialNumber ||
        expected_info.nFileIndexHigh != observed_info.nFileIndexHigh || expected_info.nFileIndexLow != observed_info.nFileIndexLow ||
        !same_path(observed.wstring(), expected.wstring()))
        fail("loaded bridge image identity mismatch");
    loaded_image_ = observed;
    return value;
}

void* Package::symbol(std::string_view name) {
    if (void* value = optional_symbol(name)) return value;
    fail("bridge symbol is absent");
}
std::filesystem::path Package::loaded_image_path() const {
    if (loaded_image_.empty()) fail("bridge image has not been identity-checked");
    return loaded_image_;
}
}  // namespace rrv::resource_package
