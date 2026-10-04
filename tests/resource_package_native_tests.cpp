#include "rrv_resource_package.h"

#include <iostream>

int main() {
    try {
        using Value = int (*)();
        for (int count = 0; count != 16; ++count) {
            auto package = rrv::resource_package::Package::Open(rrv::resource_package::rrv_resource_package_binding);
            const auto bridge = package.bridge_path();
            if (!bridge.is_absolute() || bridge.filename().empty()) return 2;
            const auto value = reinterpret_cast<Value>(package.symbol("bridge_value"));
            if (value() != 17 || package.loaded_image_path() != bridge) return 3;
        }
        auto invalid = rrv::resource_package::rrv_resource_package_binding;
        invalid.composition = "0000000000000000000000000000000000000000000000000000000000000000";
        try { (void)rrv::resource_package::Package::Open(invalid); return 4; }
        catch (const std::exception&) {}
        std::cout << "typed-package-reader-pass\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
