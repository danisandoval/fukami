// PS2Runtime::isSpecialAddress decides, for every guest load and store of the recompiled code, whether the
// access takes the slow path. It answers most addresses from the top address nibble; this test compares it
// with the range tests alone (the function as it was) on the first and last byte of every 4 KB page of the
// 32-bit address space, around every range boundary, and on 20 million random addresses.

#include "ps2_runtime.h"

#include <cstdint>
#include <cstdio>

namespace
{
// The range tests, without the nibble shortcut.
bool reference(uint32_t addr)
{
    auto inRange = [](uint32_t value, uint32_t base, uint32_t size) { return (value - base) < size; };
    if (addr >= 0xC0000000u)
        return true;
    const uint32_t phys = (addr >= 0x80000000u) ? (addr & 0x1FFFFFFFu) : addr;
    if (inRange(phys, PS2_BIOS_BASE, PS2_BIOS_SIZE))
        return true;
    if (inRange(phys, PS2_SCRATCHPAD_BASE, 0x10000000u))
        return true;
    if (inRange(phys, PS2_IO_BASE, PS2_IO_SIZE))
        return true;
    if (inRange(phys, PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE))
        return true;
    if (phys >= PS2_VU0_CODE_BASE && phys < (PS2_VU1_DATA_BASE + PS2_VU1_DATA_SIZE))
        return true;
    return false;
}
} // namespace

int main()
{
    uint64_t checked = 0, special = 0;
    int failures = 0;
    auto check = [&](uint32_t addr) {
        const bool want = reference(addr);
        ++checked;
        special += want ? 1u : 0u;
        if (PS2Runtime::isSpecialAddress(addr) != want)
        {
            if (failures < 10)
                std::fprintf(stderr, "FAIL: 0x%08x is %sspecial\n", addr, want ? "" : "not ");
            ++failures;
        }
    };
    for (uint64_t page = 0; page < 0x100000000ull; page += 0x1000u)
    {
        check(static_cast<uint32_t>(page));
        check(static_cast<uint32_t>(page + 0xFFFu));
    }
    const uint32_t edges[] = {0u, PS2_RAM_SIZE, PS2_IO_BASE, PS2_IO_BASE + PS2_IO_SIZE, PS2_VU0_CODE_BASE,
                              PS2_VU1_DATA_BASE + PS2_VU1_DATA_SIZE, PS2_GS_PRIV_REG_BASE,
                              PS2_GS_PRIV_REG_BASE + PS2_GS_PRIV_REG_SIZE, PS2_BIOS_BASE, 0x20000000u,
                              PS2_SCRATCHPAD_BASE, PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE, 0x80000000u,
                              0xA0000000u, 0xC0000000u};
    for (const uint32_t edge : edges)
        for (const uint32_t segment : {0x00000000u, 0x80000000u, 0xA0000000u})
            for (int d = -4; d <= 4; ++d)
                check(edge + segment + static_cast<uint32_t>(d));
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 20000000; ++i)
    {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        check(static_cast<uint32_t>(rng));
    }
    std::printf("special address: %llu addresses checked (%llu special), %d failures\n",
                (unsigned long long)checked, (unsigned long long)special, failures);
    return failures == 0 ? 0 : 1;
}
