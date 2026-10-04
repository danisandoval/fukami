// GS VRAM swizzle regression gate — covers the address maths the runtime
// ACTUALLY executes.
//
// Why this file exists
// --------------------
// The vendored tree carries two independent GS swizzle implementations:
//
//   * GSMem  (runtime/ps2_gs_memory.h + src/lib/ps2_gs_memory.cpp)
//     Templated PixelStorageTraits<psm> with a precomputed page lookup table.
//     This is the LIVE path: GS::GS() (ps2_gs_gpu.cpp) fills
//     m_read_vram_funcs / m_write_vram_funcs exclusively with GSMem::Read* /
//     GSMem::Write*, so every host->local upload, local-to-local copy, raster
//     write and CLUT fetch goes through it.
//
//   * GSPSMCT32 / GSPSMT8 / GSPSMT4 (runtime/ps2_gs_psm*.h)
//     Closed-form address functions. Nothing in the runtime calls them; they
//     survive only as a cross-check oracle (see the header banners).
//
// The vendored ps2xTest swizzle suite asserted against the *oracle*, so it
// could be fully green while saying nothing about the maths that runs. That
// false-confidence hazard surfaced during the B-3 girl CLUT investigation
// (2026-07-22). This file closes it: every assertion below drives GSMem and
// uses the oracle only as an independent second opinion.
//
// Structure:
//   1. probeLiveByteAddr  — recovers the byte GSMem::Write* actually touched.
//   2. golden vectors     — GS-manual layout expectations, asserted on GSMem.
//   3. differential sweep — GSMem vs oracle, O(1) per sample, plus a negative
//                           control proving the sweep can actually fail.
//   4. round-trip         — Write/Read agree for every PSM the GS dispatches.
//   5. known divergence   — the odd-TBW page-stride disagreement, pinned so a
//                           change in either implementation is noticed.

#include "runtime/ps2_gs_memory.h"

// Reference-only oracle. Not linked into any runtime code path.
#include "runtime/ps2_gs_psmct32.h"
#include "runtime/ps2_gs_psmt8.h"
#include "runtime/ps2_gs_psmt4.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using WriteFn = void (*)(u8 *, u32, u32, u32, u32, u32);
using ReadFn = u32 (*)(u8 *, u32, u32, u32, u32);

int g_failures = 0;

void check(bool cond, const char *what)
{
    if (!cond)
    {
        ++g_failures;
        std::cerr << "  FAIL: " << what << '\n';
    }
}

void checkEq(u32 actual, u32 expected, const char *what)
{
    if (actual != expected)
    {
        ++g_failures;
        std::cerr << "  FAIL: " << what << " (got " << actual << ", expected "
                  << expected << ")\n";
    }
}

std::vector<u8> &vram()
{
    static std::vector<u8> v(GSMem::MEMORY_SIZE, 0u);
    return v;
}

// Recover the byte offset a GSMem write lands on, by writing into a zeroed
// buffer and finding the byte that changed. Deliberately does not reach into
// PixelStorageTraits::Address — the point is to observe the live call.
// Linear scan over 4 MB, so keep the call count in the dozens.
u32 probeLiveByteAddr(WriteFn write, u32 bp, u32 bw, u32 x, u32 y, u32 value)
{
    auto &v = vram();
    write(v.data(), bp, bw, x, y, value);

    u32 found = 0xFFFFFFFFu;
    const uint64_t *words = reinterpret_cast<const uint64_t *>(v.data());
    const size_t wordCount = v.size() / sizeof(uint64_t);
    for (size_t i = 0; i < wordCount && found == 0xFFFFFFFFu; ++i)
    {
        if (words[i] == 0u)
        {
            continue;
        }
        for (size_t b = 0; b < sizeof(uint64_t); ++b)
        {
            if (v[i * sizeof(uint64_t) + b] != 0u)
            {
                found = static_cast<u32>(i * sizeof(uint64_t) + b);
                break;
            }
        }
    }

    if (found != 0xFFFFFFFFu)
    {
        std::memset(v.data() + found, 0, 4u);
    }
    return found;
}

// ---------------------------------------------------------------------------
// 2. Golden vectors — the GS-manual layout expectations the vendored ps2xTest
//    suite used to assert against the oracle, restated against GSMem.
// ---------------------------------------------------------------------------

void testGoldenPSMCT32()
{
    std::cout << "[live] PSMCT32 golden layout\n";
    constexpr u32 kBlock = 0u;
    constexpr u32 kBw = 1u; // one 64x32 PSMCT32 page

    struct Case { u32 x, y, byteAddr; const char *what; };
    static const Case kCases[] = {
        {0u, 0u, 0u, "PSMCT32 origin maps to byte 0"},
        {1u, 0u, 4u, "PSMCT32 x=1 advances one 32-bit pixel"},
        {0u, 1u, 8u, "PSMCT32 y=1 follows the column table's row interleave"},
        {7u, 7u, 252u, "PSMCT32 last pixel of the first 8x8 column"},
        {8u, 0u, 256u, "PSMCT32 x=8 advances to the next block"},
        {0u, 8u, 512u, "PSMCT32 y=8 advances to the block below"},
        {63u, 31u, 8188u, "PSMCT32 last pixel of a page lands at the page end"},
        {64u, 0u, 8192u, "PSMCT32 x=64 advances to the next page"},
    };

    for (const auto &c : kCases)
    {
        checkEq(probeLiveByteAddr(GSMem::WriteCT32, kBlock, kBw, c.x, c.y, 0xAABBCCDDu),
                c.byteAddr, c.what);
    }
}

void testGoldenPSMT8()
{
    std::cout << "[live] PSMT8 golden layout\n";
    constexpr u32 kBlock = 0u;
    constexpr u32 kBw = 2u; // one 128x64 PSMT8 page

    struct Case { u32 x, y, byteAddr; const char *what; };
    static const Case kCases[] = {
        {0u, 0u, 0u, "PSMT8 origin maps to byte 0"},
        {1u, 0u, 4u, "PSMT8 x=1 follows the 8-bit byte interleave"},
        {0u, 1u, 8u, "PSMT8 y=1 lands on the next row stride"},
        {0u, 2u, 33u, "PSMT8 y=2 preserves the odd-row shuffle"},
        {0u, 3u, 41u, "PSMT8 y=3 preserves the alternating block rows"},
        {15u, 15u, 255u, "PSMT8 last texel of the first 16x16 block"},
        {16u, 0u, 256u, "PSMT8 x=16 advances to the next 16x16 block"},
        {16u, 16u, 768u, "PSMT8 x=16,y=16 combines block column and block row"},
        {32u, 0u, 1024u, "PSMT8 x=32 advances to the third block column"},
        {64u, 0u, 4096u, "PSMT8 x=64 advances to the second page half"},
        {96u, 48u, 7680u, "PSMT8 lower-right interior block"},
        {127u, 63u, 8191u, "PSMT8 last texel of a 128x64 page"},
    };

    for (const auto &c : kCases)
    {
        checkEq(probeLiveByteAddr(GSMem::WriteP8, kBlock, kBw, c.x, c.y, 0xEEu),
                c.byteAddr, c.what);
    }
}

void testGoldenPSMT4()
{
    std::cout << "[live] PSMT4 golden layout\n";
    constexpr u32 kBlock = 0u;
    constexpr u32 kBw = 2u; // one 128x128 PSMT4 page

    // Expectations are nibble indices (matching the GS manual and the oracle);
    // GSMem is byte-addressed, so the probe result is compared against
    // nibble >> 1 and the surviving nibble is checked separately.
    struct Case { u32 x, y, nibble; const char *what; };
    static const Case kCases[] = {
        {0u, 0u, 0u, "PSMT4 origin maps to nibble 0"},
        {1u, 0u, 8u, "PSMT4 x=1 advances to the next packed nibble group"},
        {0u, 1u, 16u, "PSMT4 y=1 follows the manual's row packing"},
        {0u, 2u, 65u, "PSMT4 y=2 includes the odd-row permutation"},
        {0u, 3u, 81u, "PSMT4 y=3 stays in the first block's column layout"},
        {31u, 15u, 511u, "PSMT4 last texel of the first 32x16 block"},
        {32u, 0u, 1024u, "PSMT4 x=32 advances to the next swizzled block"},
        {32u, 16u, 1536u, "PSMT4 x=32,y=16 follows the second block-row permutation"},
        {64u, 0u, 4096u, "PSMT4 x=64 advances to the third block column"},
        {96u, 112u, 15872u, "PSMT4 bottom-right block origin"},
        {127u, 127u, 16383u, "PSMT4 last texel of a 128x128 page"},
        {128u, 0u, 16384u, "PSMT4 x=128 advances to the next page"},
    };

    auto &v = vram();
    for (const auto &c : kCases)
    {
        checkEq(probeLiveByteAddr(GSMem::WriteP4, kBlock, kBw, c.x, c.y, 0xFu),
                c.nibble >> 1, c.what);

        // Nibble half: write 0xF into a cleared byte and confirm it lands in
        // the half the manual predicts.
        const u32 byteAddr = c.nibble >> 1;
        v[byteAddr] = 0u;
        GSMem::WriteP4(v.data(), kBlock, kBw, c.x, c.y, 0xFu);
        const u32 expectedByte = ((c.nibble & 1u) != 0u) ? 0xF0u : 0x0Fu;
        checkEq(v[byteAddr], expectedByte, "PSMT4 nibble half matches the manual");
        v[byteAddr] = 0u;
    }
}

// ---------------------------------------------------------------------------
// 3. Differential sweep — GSMem (live) vs GSPSMx (oracle).
//
//    O(1) per sample: zero the byte the oracle names, write a unique nonzero
//    value through GSMem, and require it to show up there. If GSMem addressed
//    anywhere else the oracle byte stays zero, so a disagreement can never be
//    masked by a stale value from an earlier sample.
// ---------------------------------------------------------------------------

// oracleByteAddr returns the byte the oracle predicts; oracleShift is the
// nibble shift for 4-bit formats (0 otherwise).
bool sampleAgrees(WriteFn write, u32 oracleByteAddr, u32 oracleShift, u32 bitsPerPixel,
                  u32 bp, u32 bw, u32 x, u32 y, u32 value)
{
    auto &v = vram();
    if (oracleByteAddr + 4u >= v.size())
    {
        return true; // out of VRAM; not a meaningful sample
    }

    std::memset(v.data() + oracleByteAddr, 0, 4u);
    write(v.data(), bp, bw, x, y, value);

    u32 observed = 0u;
    std::memcpy(&observed, v.data() + oracleByteAddr, 4u);

    bool ok;
    if (bitsPerPixel == 4u)
    {
        ok = ((observed >> oracleShift) & 0x0Fu) == (value & 0x0Fu);
    }
    else if (bitsPerPixel == 8u)
    {
        ok = (observed & 0xFFu) == (value & 0xFFu);
    }
    else
    {
        ok = observed == value;
    }

    std::memset(v.data() + oracleByteAddr, 0, 4u);
    return ok;
}

// A single sweep step, shared by the real sweep and the negative control.
// `addrBias` is added to the oracle address; the real sweep passes 0 and the
// negative control passes a nonzero value that must make every sample fail.
int sweepMismatches(u32 addrBias, bool includeOddTbw, int *outSamples = nullptr)
{
    static const u32 kBps[] = {0u, 32u, 64u, 128u, 1024u, 8620u};
    static const u32 kEvenBw[] = {2u, 4u, 8u, 10u};
    static const u32 kOddBw[] = {1u, 3u, 5u};

    int mismatches = 0;
    int samples = 0;
    u32 magic = 1u;

    auto sweepPsm = [&](const char *label, WriteFn write, u32 bitsPerPixel,
                        u32 (*oracle)(u32, u32, u32, u32), u32 maxX, u32 maxY) {
        std::vector<u32> bws(std::begin(kEvenBw), std::end(kEvenBw));
        if (includeOddTbw)
        {
            bws.insert(bws.end(), std::begin(kOddBw), std::end(kOddBw));
        }

        for (u32 bp : kBps)
        {
            for (u32 bw : bws)
            {
                for (u32 y = 0u; y < maxY; ++y)
                {
                    for (u32 x = 0u; x < maxX; ++x)
                    {
                        const u32 raw = oracle(bp, bw, x, y);
                        u32 byteAddr;
                        u32 shift = 0u;
                        if (bitsPerPixel == 4u)
                        {
                            byteAddr = (raw >> 1) + addrBias;
                            shift = ((raw & 1u) != 0u) ? 4u : 0u;
                        }
                        else
                        {
                            byteAddr = raw + addrBias;
                        }

                        // Keep the payload nonzero and, for wide formats,
                        // unique per sample.
                        const u32 value = (bitsPerPixel == 4u)
                                              ? ((magic % 15u) + 1u)
                                              : (bitsPerPixel == 8u
                                                     ? ((magic % 255u) + 1u)
                                                     : (magic | 0x80000000u));
                        ++magic;
                        ++samples;

                        if (!sampleAgrees(write, byteAddr, shift, bitsPerPixel,
                                          bp, bw, x, y, value))
                        {
                            if (mismatches < 5 && addrBias == 0u)
                            {
                                std::cerr << "  FAIL: " << label << " bp=" << bp
                                          << " bw=" << bw << " x=" << x << " y=" << y
                                          << " oracleByte=" << byteAddr << '\n';
                            }
                            ++mismatches;
                        }
                    }
                }
            }
        }
    };

    sweepPsm("PSMCT32", GSMem::WriteCT32, 32u, GSPSMCT32::addrPSMCT32, 70u, 40u);
    sweepPsm("PSMT8", GSMem::WriteP8, 8u, GSPSMT8::addrPSMT8, 136u, 70u);
    sweepPsm("PSMT4", GSMem::WriteP4, 4u, GSPSMT4::addrPSMT4, 136u, 136u);

    if (outSamples != nullptr)
    {
        *outSamples = samples;
    }
    return mismatches;
}

void testDifferentialAgainstOracle()
{
    int samples = 0;
    const int mismatches = sweepMismatches(0u, /*includeOddTbw=*/false, &samples);
    std::cout << "[diff] GSMem vs GSPSMx oracle (even TBW): " << samples
              << " samples, " << mismatches << " mismatches\n";
    check(samples > 100000,
          "the differential sweep must actually cover a meaningful sample count");
    check(mismatches == 0,
          "GSMem and the GSPSMx oracle must agree on every even-TBW sample");
}

void testDifferentialNegativeControl()
{
    // Guards against the sweep being vacuously green: perturbing the oracle
    // address by one 32-bit pixel must make essentially every sample fail.
    int samples = 0;
    const int mismatches = sweepMismatches(4u, /*includeOddTbw=*/false, &samples);
    std::cout << "[diff] negative control (perturbed oracle): " << samples
              << " samples, " << mismatches << " mismatches (expected: nearly all)\n";
    check(mismatches > samples / 2,
          "the differential sweep must detect a deliberately wrong oracle address");
}

// ---------------------------------------------------------------------------
// 4. Round-trip — every PSM the GS constructor dispatches to must read back
//    what it wrote, including the paletted-in-32 formats that share PSMCT32
//    storage and must not clobber the colour bits.
// ---------------------------------------------------------------------------

void testRoundTrip()
{
    std::cout << "[live] GSMem write/read round-trip\n";
    auto &v = vram();

    struct Entry { const char *name; WriteFn w; ReadFn r; u32 mask; };
    static const Entry kEntries[] = {
        {"CT32", GSMem::WriteCT32, GSMem::ReadCT32, 0xFFFFFFFFu},
        {"CT24", GSMem::WriteCT24, GSMem::ReadCT24, 0x00FFFFFFu},
        {"CT16", GSMem::WriteCT16, GSMem::ReadCT16, 0x0000FFFFu},
        {"CT16S", GSMem::WriteCT16S, GSMem::ReadCT16S, 0x0000FFFFu},
        {"Z32", GSMem::WriteZ32, GSMem::ReadZ32, 0xFFFFFFFFu},
        {"Z24", GSMem::WriteZ24, GSMem::ReadZ24, 0x00FFFFFFu},
        {"Z16", GSMem::WriteZ16, GSMem::ReadZ16, 0x0000FFFFu},
        {"Z16S", GSMem::WriteZ16S, GSMem::ReadZ16S, 0x0000FFFFu},
        {"P8", GSMem::WriteP8, GSMem::ReadP8, 0x000000FFu},
        {"P8H", GSMem::WriteP8H, GSMem::ReadP8H, 0x000000FFu},
        {"P4", GSMem::WriteP4, GSMem::ReadP4, 0x0000000Fu},
        {"P4HL", GSMem::WriteP4HL, GSMem::ReadP4HL, 0x0000000Fu},
        {"P4HH", GSMem::WriteP4HH, GSMem::ReadP4HH, 0x0000000Fu},
    };

    static const u32 kCoords[][2] = {
        {0u, 0u}, {1u, 0u}, {0u, 1u}, {7u, 3u}, {15u, 15u},
        {31u, 17u}, {63u, 31u}, {64u, 32u}, {127u, 63u},
    };

    for (const auto &e : kEntries)
    {
        std::fill(v.begin(), v.end(), 0u);
        u32 seed = 0x13572468u;
        for (const auto &c : kCoords)
        {
            seed = seed * 1664525u + 1013904223u;
            const u32 value = seed & e.mask;
            e.w(v.data(), 64u, 4u, c[0], c[1], value);
            const u32 got = e.r(v.data(), 64u, 4u, c[0], c[1]) & e.mask;
            if (got != value)
            {
                ++g_failures;
                std::cerr << "  FAIL: " << e.name << " round-trip at (" << c[0]
                          << "," << c[1] << "): wrote " << value << ", read " << got
                          << '\n';
            }
        }
    }

    // P8H / P4HL / P4HH live in the upper byte of a PSMCT32 word. Writing an
    // index must leave the RGB bits of that word alone.
    std::fill(v.begin(), v.end(), 0u);
    GSMem::WriteCT32(v.data(), 64u, 4u, 3u, 5u, 0x00ABCDEFu);
    GSMem::WriteP8H(v.data(), 64u, 4u, 3u, 5u, 0x5Au);
    checkEq(GSMem::ReadCT32(v.data(), 64u, 4u, 3u, 5u), 0x5AABCDEFu,
            "P8H write must preserve the RGB bits of the shared PSMCT32 word");
    checkEq(GSMem::ReadP8H(v.data(), 64u, 4u, 3u, 5u), 0x5Au,
            "P8H read must return the index from the upper byte");
}

// ---------------------------------------------------------------------------
// 5. Known divergence — page stride for the 8- and 4-bit formats at odd TBW.
//
//    GSMem::PageId computes the page row as (y / pageH) * ((bw * 64) / pageW).
//    For PSMT8 pageW is 128 and for PSMT4 it is 128, so the division truncates:
//    at TBW=1 the multiplier becomes 0 and every page row collapses onto page
//    row 0. The oracle instead uses max(bw >> 1, 1), and PCSX2 uses the
//    round-up form (bw + 1) >> 1 — so all three agree only at even TBW.
//
//    This is pinned rather than fixed: changing the live VRAM address maths is
//    a renderer change, not a test change. The assertions below fail the moment
//    either implementation moves, which is the point.
//
//    Practical reach: only bites 8-/4-bit surfaces with odd TBW that are taller
//    than one page (>64 rows for PSMT8, >128 for PSMT4). TBW=1 is the common
//    case for CLUTs and small textures.
// ---------------------------------------------------------------------------

void testKnownOddTbwDivergence()
{
    std::cout << "[known] odd-TBW page stride divergence (GSMem vs oracle)\n";

    // TBW=1, PSMT8, y past the first page row: GSMem stays on page row 0.
    checkEq(probeLiveByteAddr(GSMem::WriteP8, 0u, 1u, 0u, 64u, 0xEEu), 0u,
            "GSMem PSMT8 TBW=1 collapses page row 1 onto page row 0 (known bug)");
    checkEq(GSPSMT8::addrPSMT8(0u, 1u, 0u, 64u), 8192u,
            "oracle PSMT8 TBW=1 advances one page row");

    checkEq(probeLiveByteAddr(GSMem::WriteP4, 0u, 1u, 0u, 128u, 0xFu), 0u,
            "GSMem PSMT4 TBW=1 collapses page row 1 onto page row 0 (known bug)");
    checkEq(GSPSMT4::addrPSMT4(0u, 1u, 0u, 128u) >> 1, 8192u,
            "oracle PSMT4 TBW=1 advances one page row");

    // TBW=3: GSMem and the oracle agree with each other (both floor to 1) but
    // both differ from the round-up form PCSX2 uses. Pinned so a move is seen.
    checkEq(probeLiveByteAddr(GSMem::WriteP8, 0u, 3u, 0u, 64u, 0xEEu), 8192u,
            "GSMem PSMT8 TBW=3 uses a floored page stride of 1");
    checkEq(GSPSMT8::addrPSMT8(0u, 3u, 0u, 64u), 8192u,
            "oracle PSMT8 TBW=3 also floors to a page stride of 1");

    // Even TBW: no divergence, by construction of the sweep above.
    checkEq(probeLiveByteAddr(GSMem::WriteP8, 0u, 2u, 0u, 64u, 0xEEu), 8192u,
            "GSMem PSMT8 TBW=2 advances one page row");
    checkEq(probeLiveByteAddr(GSMem::WriteP8, 0u, 4u, 0u, 64u, 0xEEu), 16384u,
            "GSMem PSMT8 TBW=4 advances two page rows");
}

} // namespace

int main()
{
    // The GS constructor does this; do it explicitly so the tests exercise
    // GSMem without standing up a whole GS.
    GSMem::InitLookupTables();

    testGoldenPSMCT32();
    testGoldenPSMT8();
    testGoldenPSMT4();
    testDifferentialAgainstOracle();
    testDifferentialNegativeControl();
    testRoundTrip();
    testKnownOddTbwDivergence();

    if (g_failures != 0)
    {
        std::cerr << "\ngs_swizzle_tests: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "\ngs_swizzle_tests: all checks passed\n";
    return 0;
}
