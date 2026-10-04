#include "rrv_m2_initial_state.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace
{
int failures = 0;
void check(bool value, const char *message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
}

int main()
{
    using rrv::m2initial::ByteRegion;
    constexpr std::array<uint8_t, 5u> hello = {'h', 'e', 'l', 'l', 'o'};
    check(rrv::m2initial::fnv1a64ForTesting(hello.data(), hello.size()) == 0xa430d84680aabd0bull,
          "byte FNV-1a vector is portable and fixed");

    constexpr std::array<uint8_t, 3u> first = {1u, 2u, 3u};
    constexpr std::array<uint8_t, 3u> second = {4u, 5u, 6u};
    const std::array<ByteRegion, 2u> ordered = {{
        {"first", first.data(), first.size()}, {"second", second.data(), second.size()},
    }};
    const std::array<ByteRegion, 2u> swapped = {{
        {"second", second.data(), second.size()}, {"first", first.data(), first.size()},
    }};
    const uint64_t a = rrv::m2initial::fingerprintRegionSequenceForTesting(ordered.data(), ordered.size());
    const uint64_t b = rrv::m2initial::fingerprintRegionSequenceForTesting(ordered.data(), ordered.size());
    const uint64_t c = rrv::m2initial::fingerprintRegionSequenceForTesting(swapped.data(), swapped.size());
    check(a == b, "explicit region serialization is repeatable");
    check(a != c, "region order is part of the canonical overall fingerprint");
    return failures == 0 ? 0 : 1;
}
