// REFERENCE ONLY -- NOT THE LIVE PATH.
//
// GSPSMCT32 is a closed-form GS swizzle oracle. No runtime code calls it:
// GS::GS() (src/lib/ps2_gs_gpu.cpp) points m_read_vram_funcs /
// m_write_vram_funcs exclusively at GSMem::Read* / GSMem::Write*
// (include/runtime/ps2_gs_memory.h), so every upload, local-to-local copy,
// raster write and CLUT fetch goes through GSMem instead.
//
// It is kept deliberately, as an INDEPENDENT second opinion: the differential
// test in the outer repo (tests/gs_swizzle_tests.cpp, target
// rrv-gs-swizzle-tests) sweeps GSMem against these functions and fails on any
// disagreement. Do not "fix" this file to match GSMem -- that would destroy
// the independence that makes the cross-check worth having.
//
// Known disagreement, pinned by that test: for odd TBW the 8-/4-bit page
// stride differs (GSMem truncates (bw*64)/pageWidth to 0 at TBW=1; this file
// clamps bw>>1 to a minimum of 1; PCSX2 rounds up as (bw+1)>>1).
//
// Asserting against this file INSTEAD of GSMem is what made the old ps2xTest
// swizzle suite green-but-meaningless (found during B-3, 2026-07-22).

#ifndef PS2_GS_PSMCT32_H
#define PS2_GS_PSMCT32_H

#include <cstdint>

namespace GSPSMCT32
{

    static constexpr uint8_t blockTable32[4][8] = {
        {0, 1, 4, 5, 16, 17, 20, 21},
        {2, 3, 6, 7, 18, 19, 22, 23},
        {8, 9, 12, 13, 24, 25, 28, 29},
        {10, 11, 14, 15, 26, 27, 30, 31},
    };

    static constexpr uint8_t columnTable32[8][8] = {
        {0, 1, 4, 5, 8, 9, 12, 13},
        {2, 3, 6, 7, 10, 11, 14, 15},
        {16, 17, 20, 21, 24, 25, 28, 29},
        {18, 19, 22, 23, 26, 27, 30, 31},
        {32, 33, 36, 37, 40, 41, 44, 45},
        {34, 35, 38, 39, 42, 43, 46, 47},
        {48, 49, 52, 53, 56, 57, 60, 61},
        {50, 51, 54, 55, 58, 59, 62, 63},
    };

    inline uint32_t addrPSMCT32(uint32_t block, uint32_t width, uint32_t x, uint32_t y)
    {
        const uint32_t pagesPerRow = (width != 0u) ? width : 1u;
        const uint32_t page = (block >> 5u) + (y >> 5u) * pagesPerRow + (x >> 6u);
        const uint32_t blockId = (block & 0x1Fu) + blockTable32[(y >> 3u) & 3u][(x >> 3u) & 7u];
        const uint32_t pageOffset = (blockId >> 5u) << 13u;
        const uint32_t localBlock = blockId & 0x1Fu;
        return (page << 13u) + pageOffset + localBlock * 256u +
               static_cast<uint32_t>(columnTable32[y & 0x7u][x & 0x7u]) * 4u;
    }

}

#endif
