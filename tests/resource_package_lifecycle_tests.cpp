#include "rrv_resource_package.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using rrv::resource_package::Binding;
using rrv::resource_package::EntryType;
using rrv::resource_package::InventoryEntry;
using rrv::resource_package::Package;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) fail("cannot write " + path.string());
    stream << text;
    if (!stream) fail("cannot finish " + path.string());
}

void write_bytes(const fs::path& path, const std::vector<char>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) fail("cannot write " + path.string());
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream) fail("cannot finish " + path.string());
}

std::vector<char> read_bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) fail("cannot read " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void wait_for(const fs::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!fs::exists(path)) {
        if (std::chrono::steady_clock::now() >= deadline) fail("timed out waiting for " + path.string());
        std::this_thread::sleep_for(10ms);
    }
}

fs::path build_root() {
    char executable[PATH_MAX]{};
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) != 0) fail("cannot resolve executable image");
    const fs::path binary = fs::canonical(executable);
    if (binary.parent_path().filename() != "bin") fail("test image is not in fixed B/bin layout");
    return binary.parent_path().parent_path();
}

int descriptor_count() {
    DIR* directory = opendir("/dev/fd");
    if (!directory) fail("cannot enumerate /dev/fd");
    int count = 0;
    while (dirent* entry = readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, "..")) ++count;
    }
    closedir(directory);
    return count;
}

void require_exclusive_lock() {
    const fs::path lock = build_root() / ".rrv-resource-package.lock";
    const int fd = open(lock.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) fail("cannot open stable lock for probe");
    const int result = flock(fd, LOCK_EX | LOCK_NB);
    const int error = errno;
    if (result == 0) flock(fd, LOCK_UN);
    close(fd);
    if (result != 0) fail("failed Package retained reader lease: " + std::string(std::strerror(error)));
}

template <typename Operation>
void expect_failure(Operation&& operation, const char* label) {
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    fail(std::string("expected failure: ") + label);
}

Binding binding_with_bridge_digest(const std::string_view digest, const uint64_t size, std::vector<InventoryEntry>& entries) {
    const Binding base = rrv::resource_package::rrv_resource_package_binding;
    entries.assign(base.inventory, base.inventory + base.inventory_count);
    bool replaced = false;
    for (auto& entry : entries) {
        if (entry.relative == base.bridge_relative) {
            entry.sha256 = digest;
            entry.size = size;
            replaced = true;
        }
    }
    if (!replaced) fail("test binding lacks bridge entry");
    Binding changed = base;
    changed.inventory = entries.data();
    return changed;
}

void run_reader() {
    Package package = Package::Open(rrv::resource_package::rrv_resource_package_binding);
    using Value = int (*)();
    const auto value = reinterpret_cast<Value>(package.symbol("bridge_value"));
    const auto image = package.loaded_image_path();
    if (value() != 17) fail("bridge returned unexpected value");
    std::cout << "reader value=17 image=" << image << '\n';
}

void hold_reader(const fs::path& ready, const fs::path& release, const fs::path& done) {
    std::string image;
    {
        Package package = Package::Open(rrv::resource_package::rrv_resource_package_binding);
        using Value = int (*)();
        const auto value = reinterpret_cast<Value>(package.symbol("bridge_value"));
        if (value() != 17) fail("bridge returned unexpected value");
        image = package.loaded_image_path().string();
        write_text(ready, "value=17 image=" + image + "\n");
        wait_for(release);
    }  // This destruction performs both dlclose and reader-lease release.
    write_text(done, "dlclose-and-lease-release image=" + image + "\n");
    std::cout << "held-reader-released image=" << image << '\n';
}

void failure_loop(const fs::path& scratch) {
    const fs::path current = build_root() / ".rrv-resource-packages/current";
    const fs::path bridge = current / "bridge.dylib";
    const fs::path metal = current / "resources/metal.metallib";
    const fs::path identity = current / "identity";
    const std::vector<char> original_bridge = read_bytes(bridge);
    const std::vector<char> original_metal = read_bytes(metal);
    const std::vector<char> original_identity = read_bytes(identity);
    fs::create_directories(scratch);

    // Warm the loader before establishing the ownership baseline.
    run_reader();
    const int baseline = descriptor_count();
    constexpr int iterations = 101;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const fs::path moved = scratch / ("bridge-" + std::to_string(iteration));
        fs::rename(bridge, moved);
        expect_failure([] { (void)Package::Open(rrv::resource_package::rrv_resource_package_binding); }, "missing bridge");
        fs::rename(moved, bridge);
        require_exclusive_lock();
        if (descriptor_count() != baseline) fail("descriptor growth after missing bridge");

        write_text(identity, "malformed identity\n");
        expect_failure([] { (void)Package::Open(rrv::resource_package::rrv_resource_package_binding); }, "malformed identity");
        write_bytes(identity, original_identity);
        require_exclusive_lock();
        if (descriptor_count() != baseline) fail("descriptor growth after malformed identity");

        write_text(metal, "hash mismatch\n");
        expect_failure([] { (void)Package::Open(rrv::resource_package::rrv_resource_package_binding); }, "hash mismatch");
        write_bytes(metal, original_metal);
        require_exclusive_lock();
        if (descriptor_count() != baseline) fail("descriptor growth after hash mismatch");

        constexpr std::string_view invalid_image = "not a Mach-O dylib\n";
        write_text(bridge, std::string(invalid_image));
        std::vector<InventoryEntry> changed_entries;
        const Binding changed = binding_with_bridge_digest(RRV_LIFECYCLE_BAD_BRIDGE_SHA, invalid_image.size(), changed_entries);
        bool opened = false;
        try {
            Package package = Package::Open(changed);
            opened = true;
            package.open_bridge();
            fail("expected malformed bridge dlopen failure");
        } catch (const std::exception& error) {
            if (!opened) fail("malformed bridge failed verification before dlopen: " + std::string(error.what()));
            if (std::string_view(error.what()).find("fixed bridge load failed") == std::string_view::npos) {
                fail("malformed bridge did not report dlopen failure: " + std::string(error.what()));
            }
            if (iteration == 0) std::cout << "observed-dlopen-failure=" << error.what() << '\n';
        }
        write_bytes(bridge, original_bridge);
        require_exclusive_lock();
        if (descriptor_count() != baseline) fail("descriptor growth after dlopen failure");

        expect_failure([] { Package package = Package::Open(rrv::resource_package::rrv_resource_package_binding); (void)package.symbol("missing_bridge_symbol"); }, "dlsym failure");
        require_exclusive_lock();
        if (descriptor_count() != baseline) fail("descriptor growth after dlsym failure");
    }
    std::cout << "failure-loop-pass iterations=" << iterations << " baseline-fds=" << baseline
              << " final-fds=" << descriptor_count() << '\n';
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "read") {
            run_reader();
        } else if (argc == 5 && std::string_view(argv[1]) == "hold") {
            hold_reader(argv[2], argv[3], argv[4]);
        } else if (argc == 3 && std::string_view(argv[1]) == "failure-loop") {
            failure_loop(argv[2]);
        } else {
            fail("usage: lifecycle-reader read | hold <ready> <release> <done> | failure-loop <scratch>");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
