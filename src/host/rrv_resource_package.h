#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace rrv::resource_package {

// What a directory, lock or package root is held by: a file descriptor on macOS and Linux, a HANDLE on Windows.
#if defined(_WIN32)
using NativeHandle = void*;
inline const NativeHandle kInvalidHandle = reinterpret_cast<NativeHandle>(~static_cast<uintptr_t>(0));
#else
using NativeHandle = int;
inline constexpr NativeHandle kInvalidHandle = -1;
#endif

enum class EntryType : uint8_t { file, directory };

// This is generated at configuration time. It deliberately contains neither a
// build-root pathname nor original-build inode identities.
struct InventoryEntry {
    std::string_view relative;
    EntryType type;
    uint64_t size;
    std::string_view sha256;
};

struct Binding {
    std::string_view version;
    std::string_view composition;
    std::string_view bridge_relative;
    const InventoryEntry* inventory;
    size_t inventory_count;
};

extern const Binding rrv_resource_package_binding;

class ReaderLease {
public:
    ReaderLease() = default;
    explicit ReaderLease(NativeHandle lock_handle);
    ~ReaderLease();
    ReaderLease(const ReaderLease&) = delete;
    ReaderLease& operator=(const ReaderLease&) = delete;
    ReaderLease(ReaderLease&& other) noexcept;
    ReaderLease& operator=(ReaderLease&& other) noexcept;

private:
    NativeHandle fd_ = kInvalidHandle;
};

class Package {
public:
    // Uses the native process-image API, then accepts only B/bin/<image> (Windows: also Fukami.exe beside resources/).
    // The returned object keeps its shared lease through its destructor.
    static Package Open(const Binding& binding);
    ~Package();
    Package(const Package&) = delete;
    Package& operator=(const Package&) = delete;
    Package(Package&& other) noexcept;
    Package& operator=(Package&& other) noexcept;

    [[nodiscard]] std::filesystem::path bridge_path() const;
    [[nodiscard]] std::filesystem::path resource_path(std::string_view relative) const;
    void open_bridge();
    [[nodiscard]] void* symbol(std::string_view name);
    // Returns null only when the bridge genuinely does not export `name`.
    // A resolved symbol always receives the same dladdr identity validation.
    [[nodiscard]] void* optional_symbol(std::string_view name);
    // Records the canonical `dladdr` image only after a symbol identity check.
    [[nodiscard]] std::filesystem::path loaded_image_path() const;

private:
    Package(Binding binding, std::filesystem::path root, NativeHandle package_fd, ReaderLease lease);
    void close() noexcept;
    Binding binding_{};
    std::filesystem::path root_;
    NativeHandle package_fd_ = kInvalidHandle;
    void* image_ = nullptr;
    std::filesystem::path loaded_image_;
    ReaderLease lease_;
};

}  // namespace rrv::resource_package
