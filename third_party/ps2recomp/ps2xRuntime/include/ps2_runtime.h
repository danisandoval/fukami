#include "rrv_guest_rtc.h"
#include "rrv_guest_scheduler.h"
#include "rrv_temporal_owner.h"
// RRV product SDL host overlay
#ifndef PS2_RUNTIME_H
#define PS2_RUNTIME_H
#include <cstddef>
#include <cstdint>

// Gate-8 audio hooks (scripts/gate8_audio_overlay.py). Null unless the
// candidate links the host sound driver; host-only, guest-time ordered.
struct R5900Context;
class PS2Runtime;
struct Gate8AudioHooksV1
{
    // Returns 0 = not handled, 1 = completed now, 2 = completion deferred.
    int (*sifCallRpc)(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, uint32_t sid,
                      uint32_t clientPtr, uint32_t rpcNum, uint32_t mode, uint32_t sendBuf,
                      uint32_t sendSize, uint32_t recvBuf, uint32_t recvSize, uint32_t endFunc,
                      uint32_t endParam) = nullptr;
    void (*iopWrite)(uint32_t addr, const uint8_t* src, size_t size) = nullptr;
    void (*service)(PS2Runtime* runtime, R5900Context* ctx) = nullptr;
    // True when `service`, called at a full checkpoint at this EE cycle, would do nothing. The sound
    // driver is serviced at full checkpoints only, about once a guest millisecond, so code that
    // leaves a full checkpoint out (gate3PlainMmioQuietV1) must ask first: otherwise the reply
    // or callback it would have delivered arrives at a later instruction.
    bool (*idle)(PS2Runtime* runtime, uint64_t eeCycle) = nullptr;
};
extern Gate8AudioHooksV1 g_gate8AudioHooksV1;
namespace ps2_syscalls { void gate8SetRpcBusyV1(uint32_t clientPtr, bool busy); uint32_t gate8RpcServerV1(uint32_t sid); }
namespace ps2_stubs {
bool gate8WriteIopHeapNoHookV1(uint32_t addr, const uint8_t* src, size_t size);
bool gate8ReserveIopHeapV1(uint32_t addr, uint32_t size); // keeps the allocator away from [addr, addr+size)
}

#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <map>
#include <optional>
#include <utility>
#include <string>
#include <functional>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX instructions
#include <smmintrin.h> // For SSE4.1 instructions
#endif
#include <atomic>
#include <array>
#include <mutex>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <stdexcept>

#include "ps2_log.h"
#include "rrv_gs_backend.h"
#include "runtime/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_iop.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_audio.h"
#include "runtime/ps2_pad.h"
#include "rrv_guest_terminal_outcome.h"

enum PS2Exception
{
    EXCEPTION_TLB_REFILL = 0x02,          // TLB refill/load exception
    EXCEPTION_ADDRESS_ERROR_LOAD = 0x04,  // Address error on load
    EXCEPTION_ADDRESS_ERROR_STORE = 0x05, // Address error on store
    EXCEPTION_SYSCALL = 0x08,             // SYSCALL instruction
    EXCEPTION_BREAKPOINT = 0x09,          // BREAK instruction
    EXCEPTION_RESERVED_INSTRUCTION = 0x0A,
    EXCEPTION_INTEGER_OVERFLOW = 0x0C, // From MIPS spec
    EXCEPTION_TRAP = 0x0D,             // Trap instruction condition met
};

// PS2 CPU context (R5900)
struct alignas(16) R5900Context
{
    // General Purpose Registers (128-bit)
    __m128i r[32]; // Main registers

    // Control registers
    uint32_t pc;         // Program counter
    uint64_t insn_count; // Instruction counter
    uint64_t hi, lo;     // HI/LO registers for mult/div results
    uint64_t hi1, lo1;   // Secondary HI/LO registers for MULT1/DIV1
    uint32_t sa;         // Shift amount register

    // VU0 registers (when used in macro mode)
    __m128 vu0_vf[32];        // VU0 vector float registers
    uint16_t vi[16];          // VU0 vector integer registers
    float vu0_q;              // VU0 Q register (quotient)
    float vu0_p;              // VU0 P register (EFU result)
    float vu0_i;              // VU0 I register (integer value)
    __m128 vu0_r;             // VU0 R register
    __m128 vu0_acc;           // VU0 ACC accumulator register
    uint16_t vu0_status;      // VU0 status register
    uint32_t vu0_mac_flags;   // VU0 MAC flags
    uint32_t vu0_clip_flags;  // VU0 clipping flags
    uint32_t vu0_clip_flags2; // VU0 clipping flags
    uint32_t vu0_cmsar0;      // VU0 microprogram start address
    uint32_t vu0_cmsar1;      // VU0 microprogram start address
    uint32_t vu0_cmsar2;      // VU0 microprogram start address
    uint32_t vu0_cmsar3;      // VU0 microprogram start address
    uint32_t vu0_vpu_stat;
    uint32_t vu0_vpu_stat2; // extra VPU status (used by CR_VPU_STAT2)
    uint32_t vu0_vpu_stat3; // extra VPU status 3
    uint32_t vu0_vpu_stat4; // extra VPU status 4
    uint32_t vu0_tpc;       // TPC (VU0 PC)
    uint32_t vu0_tpc2;      // second TPC
    uint32_t vu0_fbrst;     // VIF/VU reset register
    uint32_t vu0_fbrst2;    // FBRST2
    uint32_t vu0_fbrst3;    // FBRST3
    uint32_t vu0_fbrst4;    // FBRST4
    uint32_t vu0_itop;
    uint32_t vu0_top;
    uint32_t vu0_info;
    uint32_t vu0_xitop; // VU0 XITOP - input ITOP for VIF/VU sync
    uint32_t vu0_pc;

    float vu0_cf[4]; // VU0 FMAC control floating-point registers

    // COP0 System control registers
    uint32_t cop0_index;
    uint32_t cop0_random;
    uint32_t cop0_entrylo0;
    uint32_t cop0_entrylo1;
    uint32_t cop0_context;
    uint32_t cop0_pagemask;
    uint32_t cop0_wired;
    uint32_t cop0_badvaddr;
    uint32_t cop0_count;
    uint32_t cop0_entryhi;
    uint32_t cop0_compare;
    uint32_t cop0_status;
    uint32_t cop0_cause;
    uint32_t cop0_epc;
    uint32_t cop0_prid;
    uint32_t cop0_config;
    uint32_t cop0_badpaddr;
    uint32_t cop0_debug;
    uint32_t cop0_perf;
    uint32_t cop0_taglo;
    uint32_t cop0_taghi;
    uint32_t cop0_errorepc;

    // LL/SC reservation state (not part of COP0 Status bits).
    uint32_t llbit;
    uint32_t lladdr;

    // Delay slot state tracking
    bool in_delay_slot;
    uint32_t branch_pc;

    // COP2 control registers (VU0 integer + control)
    uint32_t cop2_ccr[32];

    // FPU registers (COP1)
    float f[32];
    uint32_t fcr31; // Control/status register

    R5900Context()
    {
        std::memset(this, 0, sizeof(*this));

        // Initialize VU0 registers
        vu0_q = 1.0f; // Q register usually initialized to 1.0

        // Reset COP0 registers
        cop0_random = 47; // Start at maximum value
        // cop0_status = 0x400000; // BEV set, ERL clear, kernel mode
        // 0x00400000 = BEV (Boot Exception Vectors).
        // 0x00000000 = Normal mode (after BIOS handoff).
        cop0_status = 0x00000000;
        cop0_prid = 0x00002e20; // CPU ID for R5900

        in_delay_slot = false;
        branch_pc = 0;
    }

    void dump() const
    {
        std::ios_base::fmtflags flags = std::cout.flags();
        std::cout << std::hex << std::setfill('0');
        std::cout << "--- R5900 Context Dump ---\n";
        std::cout << "PC: 0x" << std::setw(8) << pc << "\n";
        std::cout << "HI: 0x" << std::setw(8) << hi << " LO: 0x" << std::setw(8) << lo << "\n";
        std::cout << "HI1:0x" << std::setw(8) << hi1 << " LO1:0x" << std::setw(8) << lo1 << "\n";
        std::cout << "SA: 0x" << std::setw(8) << sa << "\n";
        for (int i = 0; i < 32; ++i)
        {
            std::cout << "R" << std::setw(2) << std::dec << i << ": 0x" << std::hex
                      << std::setw(8) << static_cast<uint32_t>(_mm_extract_epi32(r[i], 3))
                      << std::setw(8) << static_cast<uint32_t>(_mm_extract_epi32(r[i], 2)) << "_"
                      << std::setw(8) << static_cast<uint32_t>(_mm_extract_epi32(r[i], 1))
                      << std::setw(8) << static_cast<uint32_t>(_mm_extract_epi32(r[i], 0)) << "\n";
        }
        std::cout << "Status: 0x" << std::setw(8) << cop0_status
                  << " Cause: 0x" << std::setw(8) << cop0_cause
                  << " EPC: 0x" << std::setw(8) << cop0_epc << "\n";
        std::cout << "--- End Context Dump ---\n";
        std::cout.flags(flags); // Restore format flags
    }

    ~R5900Context() = default;
};

inline uint32_t getRegU32(const R5900Context *ctx, int reg)
{
    // Check if reg is valid (0-31)
    if (reg < 0 || reg > 31)
        return 0;
    if (reg == 0)
        return 0;
    return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[reg], 0));
}

inline void setReturnU32(R5900Context *ctx, uint32_t value)
{
    // R5900 sign-extends 32-bit results into 64-bit GPR, even for unsigned values.
    ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value))); // $v0
}

inline void setReturnS32(R5900Context *ctx, int32_t value)
{
    // Signed 32-bit return should be sign-extended when observed as 64-bit.
    ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(value)); // $v0
}

inline void setReturnU64(R5900Context *ctx, uint64_t value)
{
    // Keep both conventions: full 64-bit value in $v0 and high 32-bit in $v1.
    ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    ctx->r[3] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<uint32_t>(value >> 32)));
}

inline constexpr uint32_t PS2_PATH_WATCH_ADDR = 0x01EFFFA0u;
inline constexpr uint32_t PS2_PATH_WATCH_BYTES = 0x200u;

// The write trap is ARMED BY RRV_PATH_WATCH AND OTHERWISE OFF.
//
// It used to be armed in every build: ps2TraceGuestWrite() runs on EVERY guest
// store the recompiler emits, and with no arming flag each of those stores paid
// two function-local-static guards (ps2PathWatchWindowBase/WindowBytes, both
// getenv-cached) plus the range test, forever, to compare against a window the
// game normally never touches.  A profile of the shipping build found
// ps2TraceGuestWrite and ps2PathWatchIntersects among the top self-time symbols
// on the guest thread -- pure diagnostic tax in the `ee` bucket
// (docs/TESTING.md T-EE-PATHWATCH).
//
// A namespace-scope inline variable, NOT a function-local static: the latter
// costs a guard load and a branch at every use, which is exactly what we are
// removing.  Reading it after main() has started is safe; nothing here runs
// during static initialisation.
inline const bool g_ps2PathWatchArmed = []
{
    const char *v = std::getenv("RRV_PATH_WATCH");
    return v != nullptr && v[0] != '\0';
}();

// B-3: the watched window used to be fixed at compile time, which made this
// otherwise-complete write trap (it already carries pc/ra/sp) unusable for any
// address but one.  RRV_PATH_WATCH=<hexaddr>[:<hexbytes>] retargets it at run
// time; unset keeps the historical window exactly -- and now also leaves the
// trap disarmed, so the historical window only matters once someone asks for it.
inline uint32_t ps2PathWatchWindowBase()
{
    static const uint32_t base = []
    {
        if (const char *v = std::getenv("RRV_PATH_WATCH"))
        {
            return static_cast<uint32_t>(std::strtoul(v, nullptr, 16));
        }
        return PS2_PATH_WATCH_ADDR;
    }();
    return base;
}

inline uint32_t ps2PathWatchWindowBytes()
{
    static const uint32_t bytes = []
    {
        if (const char *v = std::getenv("RRV_PATH_WATCH"))
        {
            if (const char *colon = std::strchr(v, ':'))
            {
                const uint32_t n = static_cast<uint32_t>(std::strtoul(colon + 1, nullptr, 16));
                if (n != 0u)
                    return n;
            }
        }
        return PS2_PATH_WATCH_BYTES;
    }();
    return bytes;
}
inline constexpr uint32_t PS2_PATH_WATCH_MAX_LOGS = 4096u;
inline std::atomic<uint32_t> g_ps2PathWatchLogCount{0};

inline uint32_t ps2PathWatchPhysAddr()
{
    return ps2PathWatchWindowBase() & PS2_RAM_MASK;
}

inline bool ps2PathWatchIntersects(uint32_t writeAddr, uint32_t writeSize)
{
    if (!g_ps2PathWatchArmed)
    {
        return false;
    }
    const uint64_t writeStart = writeAddr;
    const uint64_t writeEnd = writeStart + static_cast<uint64_t>(writeSize);
    const uint64_t watchStart = ps2PathWatchPhysAddr();
    const uint64_t watchEnd = watchStart + static_cast<uint64_t>(ps2PathWatchWindowBytes());
    return writeEnd > watchStart && writeStart < watchEnd;
}

// RRV_PATH_WATCH_HIST=1 — histogram mode. Instead of logging every write (and
// stopping at PS2_PATH_WATCH_MAX_LOGS), count writes per writer PC and report
// per-interval deltas. This is the apples-to-apples counterpart of the scratch
// PCSX2 build's RRV_GT_WATCH probe, which aggregates hardware's writers to the
// same guest range the same way; comparing the two histograms is how a
// "hardware rewrites this every frame and we do not" claim gets located.
inline bool ps2PathWatchHistEnabled()
{
    static const bool on = []
    {
        const char *v = std::getenv("RRV_PATH_WATCH_HIST");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

inline std::map<uint32_t, std::pair<uint64_t, uint64_t>> &ps2PathWatchHist()
{
    static std::map<uint32_t, std::pair<uint64_t, uint64_t>> m;  // pc -> {count, previously reported}
    return m;
}

inline void ps2PathWatchHistNote(uint32_t pc)
{
    ps2PathWatchHist()[pc].first++;
}

inline void ps2PathWatchHistDump(const char *why)
{
    if (!ps2PathWatchHistEnabled())
    {
        return;
    }
    auto &m = ps2PathWatchHist();
    uint64_t total = 0, delta = 0;
    for (const auto &kv : m)
    {
        total += kv.second.first;
        delta += kv.second.first - kv.second.second;
    }
    std::fprintf(stderr, "[watch:hist] %s writers=%zu total=%llu delta=%llu\n", why, m.size(),
                 (unsigned long long)total, (unsigned long long)delta);
    for (auto &kv : m)
    {
        const uint64_t d = kv.second.first - kv.second.second;
        kv.second.second = kv.second.first;
        if (d == 0u)
        {
            continue;
        }
        std::fprintf(stderr, "  pc=%08x delta=%llu count=%llu\n", kv.first, (unsigned long long)d,
                     (unsigned long long)kv.second.first);
    }
}

inline void ps2PathWatchDumpPrefix(const uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }

    const uint32_t base = ps2PathWatchPhysAddr();
    auto flags = std::cout.flags();
    std::cout << " buf=" << std::hex;
    for (uint32_t i = 0; i < 16u; ++i)
    {
        const uint32_t addr = (base + i) & PS2_RAM_MASK;
        std::cout << static_cast<uint32_t>(rdram[addr]);
        if (i + 1u < 16u)
        {
            std::cout << '.';
        }
    }
    std::cout.flags(flags);
}

inline uint8_t ps2PathWatchExtractByteFromWrite(uint32_t writeAddr, uint32_t watchAddr, uint64_t valueLo, uint64_t valueHi)
{
    const uint32_t byteIndex = watchAddr - writeAddr;
    if (byteIndex < 8u)
    {
        return static_cast<uint8_t>((valueLo >> (byteIndex * 8u)) & 0xFFu);
    }
    return static_cast<uint8_t>((valueHi >> ((byteIndex - 8u) * 8u)) & 0xFFu);
}

// The armed trap (RRV_PATH_WATCH). Out of line on purpose: see ps2TraceGuestWrite.
__attribute__((noinline)) inline void ps2TraceGuestWriteArmed(uint8_t *rdram,
                               uint32_t guestAddr,
                               uint32_t size,
                               uint64_t valueLo,
                               uint64_t valueHi,
                               const char *op,
                               const R5900Context *ctx)
{
    if (!rdram || size == 0u)
    {
        return;
    }

    const uint32_t writeAddr = guestAddr & PS2_RAM_MASK;
    if (!ps2PathWatchIntersects(writeAddr, size))
    {
        return;
    }

    if (ps2PathWatchHistEnabled())
    {
        ps2PathWatchHistNote(ctx ? ctx->pc : 0u);
        return;
    }

    const uint32_t logIndex = g_ps2PathWatchLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logIndex >= PS2_PATH_WATCH_MAX_LOGS)
    {
        return;
    }

    const uint32_t watchAddr = ps2PathWatchPhysAddr();
    const bool touchesFirstByte = (watchAddr >= writeAddr) && (watchAddr < writeAddr + size);
    const uint8_t oldByte = rdram[watchAddr];
    const uint8_t newByte = touchesFirstByte ? ps2PathWatchExtractByteFromWrite(writeAddr, watchAddr, valueLo, valueHi) : oldByte;

    const uint32_t pc = ctx ? ctx->pc : 0u;
    const uint32_t ra = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) : 0u;
    const uint32_t sp = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) : 0u;
    const uint32_t s1 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0)) : 0u;
    const uint32_t s3 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[19], 0)) : 0u;
    const uint32_t phase = static_cast<uint32_t>(rdram[0x334E94u]) |
                           (static_cast<uint32_t>(rdram[0x334E95u]) << 8) |
                           (static_cast<uint32_t>(rdram[0x334E96u]) << 16) |
                           (static_cast<uint32_t>(rdram[0x334E97u]) << 24);

    auto flags = std::cout.flags();
    std::cout << "[watch:path-write] #" << (logIndex + 1u)
              << " op=" << op
              << " addr=0x" << std::hex << writeAddr
              << " size=0x" << size
              << " pc=0x" << pc
              << " ra=0x" << ra
              << " sp=0x" << sp
              << " s1=0x" << s1
              << " s3=0x" << s3
              << " phase=0x" << phase
              << " vLo=0x" << valueLo;
    if (size > 8u)
    {
        std::cout << " vHi=0x" << valueHi;
    }
    if (touchesFirstByte)
    {
        std::cout << " firstByte:" << static_cast<uint32_t>(oldByte)
                  << "->" << static_cast<uint32_t>(newByte);
        if (oldByte != 0u && newByte == 0u)
        {
            std::cout << " (ZEROED)";
        }
    }
    ps2PathWatchDumpPrefix(rdram);
    std::cout.flags(flags);
    std::cout << std::endl;
}

// Runs on EVERY guest store of the recompiled code, so it is one test of the
// arming flag and nothing else. The trap itself stays out of line: as one
// inline function it was too large to inline, and each store paid a call for
// that one test (380 call sites in the hottest native function, 0.17 ms per
// VBlank start on the Steam Deck profile of 2026-10-02).
__attribute__((always_inline)) inline void ps2TraceGuestWrite(uint8_t *rdram,
                               uint32_t guestAddr,
                               uint32_t size,
                               uint64_t valueLo,
                               uint64_t valueHi,
                               const char *op,
                               const R5900Context *ctx)
{
    if (__builtin_expect(g_ps2PathWatchArmed, false))
        ps2TraceGuestWriteArmed(rdram, guestAddr, size, valueLo, valueHi, op, ctx);
}

inline void ps2TraceGuestRangeWrite(uint8_t *rdram,
                                    uint32_t guestAddr,
                                    uint32_t size,
                                    const char *op,
                                    const R5900Context *ctx)
{
    // First test, before anything else touches memory: this runs on every guest
    // store in the program.
    if (!g_ps2PathWatchArmed || !rdram || size == 0u)
    {
        return;
    }

    const uint32_t writeAddr = guestAddr & PS2_RAM_MASK;
    if (!ps2PathWatchIntersects(writeAddr, size))
    {
        return;
    }

    const uint32_t logIndex = g_ps2PathWatchLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logIndex >= PS2_PATH_WATCH_MAX_LOGS)
    {
        return;
    }

    const uint32_t pc = ctx ? ctx->pc : 0u;
    const uint32_t ra = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) : 0u;
    const uint32_t sp = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) : 0u;
    const uint8_t firstByte = rdram[ps2PathWatchPhysAddr()];

    auto flags = std::cout.flags();
    std::cout << "[watch:path-range] #" << (logIndex + 1u)
              << " op=" << op
              << " addr=0x" << std::hex << writeAddr
              << " size=0x" << size
              << " pc=0x" << pc
              << " ra=0x" << ra
              << " sp=0x" << sp
              << " firstByte=" << static_cast<uint32_t>(firstByte);
    ps2PathWatchDumpPrefix(rdram);
    std::cout.flags(flags);
    std::cout << std::endl;
}

struct PS2SoundDriverCompatLayout
{
    uint32_t primarySeCheckAddr = 0;
    uint32_t primaryMidiCheckAddr = 0;
    uint32_t fallbackSeCheckAddr = 0;
    uint32_t fallbackMidiCheckAddr = 0;
    uint32_t busyFlagAddr = 0;
    std::array<uint32_t, 4> completionCallbacks{};
    std::array<uint32_t, 2> clearBusyCallbacks{};

    [[nodiscard]] bool hasChecksumTables() const
    {
        return primarySeCheckAddr != 0u || primaryMidiCheckAddr != 0u ||
               fallbackSeCheckAddr != 0u || fallbackMidiCheckAddr != 0u;
    }

    [[nodiscard]] bool matchesCompletionCallback(uint32_t addr) const
    {
        for (const uint32_t candidate : completionCallbacks)
        {
            if (candidate != 0u && candidate == addr)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool matchesClearBusyCallback(uint32_t addr) const
    {
        for (const uint32_t candidate : clearBusyCallbacks)
        {
            if (candidate != 0u && candidate == addr)
            {
                return true;
            }
        }
        return false;
    }
};

struct PS2DtxCompatLayout
{
    uint32_t rpcSid = 0;
    uint32_t urpcObjBase = 0;
    uint32_t urpcObjLimit = 0;
    uint32_t urpcObjStride = 0x20u;
    uint32_t urpcFnTableBase = 0;
    uint32_t urpcObjTableBase = 0;
    uint32_t dispatcherFuncAddr = 0;

    [[nodiscard]] bool isConfigured() const
    {
        return rpcSid != 0u;
    }

    [[nodiscard]] bool hasUrpcObjectRange() const
    {
        return urpcObjBase != 0u && urpcObjLimit > urpcObjBase && urpcObjStride != 0u;
    }

    [[nodiscard]] bool hasUrpcTables() const
    {
        return urpcFnTableBase != 0u && urpcObjTableBase != 0u;
    }

    [[nodiscard]] bool isUrpcRpc(uint32_t sid, uint32_t rpcNum) const
    {
        return isConfigured() && sid == rpcSid && rpcNum >= 0x400u && rpcNum < 0x500u;
    }
};

class PS2Runtime
{
public:
    struct IoPaths
    {
        std::filesystem::path elfPath;
        std::filesystem::path elfDirectory;
        std::filesystem::path hostRoot;
        std::filesystem::path cdRoot;
        std::filesystem::path mcRoot;
        std::filesystem::path cdImage;
    };

    PS2Runtime();
    ~PS2Runtime();

    // Configured before initialize()/GSopen. Native handles remain opaque
    // borrowed values in the RRV-owned backend surface descriptor.
    struct HostPresentationConfig
    {
        rrv::gsbackend::InitializeOptions backend{};
        void *context = nullptr;
        void (*pumpEvents)(void *) = nullptr;
        bool (*shouldClose)(void *) = nullptr;
        bool (*currentSurface)(void *, rrv::gsbackend::NativeSurface *) = nullptr;
    };

    void setHostPresentationConfig(const HostPresentationConfig &config);

    bool initialize(const char *title = "PS2 Game");
    bool syncCoreSubsystems();
    bool loadELF(const std::string &elfPath);
    void run();

    // The legacy GS remains constructed for VU integration and diagnostic
    // selection. When RRV_GS_BACKEND=pcsx2 successfully loads the optional
    // bridge, post-arbitration GIF packets and host presentation use that
    // backend instead. These helpers are intentionally runtime-owned so
    // src/main.cpp needs no PCSX2-specific lifecycle knowledge.
    bool latchActiveGsBackendPresentationFrame();
    // True only when the optional PCSX2 GS bridge owns rendering.  HLE callers
    // use this to retain RRV register bookkeeping while avoiding duplicate
    // legacy raster work.
    bool activeGsBackend() const;
    bool directGpuPresentationActive() const;
    bool presentationStats(rrv::gsbackend::PresentationStats &stats, std::string *error);
    uint64_t gsFieldTransitionCount() const { return m_gsBackend.fieldTransitionCount(); }
#if !defined(PS2X_RRV_FIELD_ONLY)
    // True only for an active PCSX2 bridge that explicitly accepted the
    // progressive FullFrame capability.  Used during startup to arm the
    // decode-only IR shadow before the first guest packet arrives.
    bool activeGsBackendUsesFullFrame() const;
#endif
    bool copyActiveGsBackendPresentationFrame(std::vector<uint8_t> &outPixels,
                                              uint32_t &outWidth,
                                              uint32_t &outHeight,
                                              uint64_t *outLiveVsyncTick = nullptr,
                                              bool *outNewLivePresent = nullptr);
    // Called by the emulated-vblank worker once per field. It is deliberately
    // separate from host texture upload so PCSX2 observes every GS field even
    // when the frontend presents less frequently.
    void advanceActiveGsBackendField(uint64_t fieldIndex);
    // Gate-4 VU1+GS owner stream (scripts/gate4_owner_stream_overlay.py).
    enum class Vu1GsModeV1 : uint8_t { Inline, StreamInline, OwnerSync, OwnerAsync };
    void gate4StoreReadbackV1(uint64_t csr, uint64_t siglblid);
    void gate4PublishOwnerV1();
    void gate4FenceV1();
    void gate4GuestGsWriteV1(uint32_t physical);
    void gate4ConfigureOwnerStreamV1();
    void gate4ShutdownOwnerStreamV1();
    void gate4AdvanceFieldOwnerV1(std::array<uint64_t, 19u> regs19, uint64_t fieldIndex, uint32_t fieldParity);
    bool gate4FieldDeferrableV1();
    void gate4FinishFieldV1(const std::array<uint64_t, 19u> &regs19, uint64_t fieldIndex);
    Vu1GsModeV1 m_vu1gsModeV1 = Vu1GsModeV1::Inline;
    uint64_t m_ownerCsrV1 = 0u, m_ownerSiglblidV1 = 0u;
    bool m_ownerCsrValidV1 = false, m_ownerSiglblidValidV1 = false;
    uint64_t m_vu1gsStressV1 = 0u, m_vu1gsSyncWaitsV1 = 0u, m_vu1gsFencesV1 = 0u;
    // Gate-5 split: field transitions queued to rrv-gs-owner without a wait
    // (Backend::vsyncDeferred). The count is written on rrv-vu1 only.
    bool m_vu1gsDeferFieldV1 = false;
    uint64_t m_vu1gsDeferredFieldsV1 = 0u;
    // HLE helpers which synthesize GS state outside GifArbiter mirror the
    // exact A+D register sequence to the active backend. `pairs` is an array
    // of {value, register} uint64_t pairs.
    void submitActiveGsBackendRegisterPairs(const uint64_t *pairs, uint32_t pairCount);
    // HLE local-to-host transfers own their guest destination in RRV; PCSX2
    // supplies only the source bytes from its GS local memory.
    bool readActiveGsBackendLocalMemory(uint8_t *dst, uint32_t byteCount,
                                        uint64_t bitbltbuf, uint64_t trxpos,
                                        uint64_t trxreg);
    // Result-boundary admission shared by direct reads and HLEs before
    // they can submit a native local-to-host transaction.
    void preflightActiveGsBackendLocalMemoryRead();
    // Complete 4 MiB GS local-memory state used by RRV snapshots. These helpers
    // acquire the guest-execution lock, so snapshot callers share one
    // serialized PCSX2 GS context with GIF dispatch and vblank transitions.
    bool snapshotActiveGsBackendLocalMemory(std::vector<uint8_t> &outBytes);
    bool restoreActiveGsBackendLocalMemory(const uint8_t *bytes, uint32_t byteCount);

    using RecompiledFunction = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    class GuestExecutionScope
    {
    public:
        explicit GuestExecutionScope(PS2Runtime *runtime);
        ~GuestExecutionScope();

        GuestExecutionScope(const GuestExecutionScope &) = delete;
        GuestExecutionScope &operator=(const GuestExecutionScope &) = delete;

    private:
        PS2Runtime *m_runtime = nullptr;
    };

    class GuestExecutionReleaseScope
    {
    public:
        explicit GuestExecutionReleaseScope(PS2Runtime *runtime);
        ~GuestExecutionReleaseScope();

        GuestExecutionReleaseScope(const GuestExecutionReleaseScope &) = delete;
        GuestExecutionReleaseScope &operator=(const GuestExecutionReleaseScope &) = delete;

    private:
        PS2Runtime *m_runtime = nullptr;
        uint32_t m_depth = 0u;
    };

    // Prevent generated loop back-edges from returning to the dispatcher while
    // a guest packet builder owns transient DMA-chain state.  The outer guest
    // mutex still remains held; this only keeps one recompiled function call
    // contiguous instead of allowing another guest thread to run between its
    // resumable loop chunks.
    class BackEdgeYieldSuppressionScope
    {
    public:
        BackEdgeYieldSuppressionScope() noexcept;
        ~BackEdgeYieldSuppressionScope();

        BackEdgeYieldSuppressionScope(const BackEdgeYieldSuppressionScope &) = delete;
        BackEdgeYieldSuppressionScope &operator=(const BackEdgeYieldSuppressionScope &) = delete;
    };

    void registerFunction(uint32_t address, RecompiledFunction func);
    RecompiledFunction lookupFunction(uint32_t address);
    bool hasFunction(uint32_t address) const;
    // Diagnostic only (RRV_FN_HITS=all): a snapshot of every registered guest
    // entry point, so the caller can trampoline the whole table without
    // iterating it while registerFunction() mutates it.
    std::vector<uint32_t> allFunctionAddresses() const
    {
        std::vector<uint32_t> out;
        out.reserve(m_functionTable.size());
        for (const auto &kv : m_functionTable)
        {
            out.push_back(kv.first);
        }
        return out;
    }

    static const IoPaths &getIoPaths();
    static void setIoPaths(const IoPaths &paths);
    static void configureIoPathsFromElf(const std::string &elfPath);

    void SignalException(R5900Context *ctx, PS2Exception exception);

    void executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address);
    void vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address);
    // VU0 burst: executeVU0Microprogram's register hand-over done once for a run of microprograms
    // (ps2_runtime.cpp, "VU0 burst"). For the native builders in src/product.
    bool vu0BurstAvailableV1() const;
    VU1State &vu0BurstBeginV1(const R5900Context *ctx);
    void vu0BurstRunV1(const R5900Context *ctx, uint32_t address);
    void vu0BurstEndV1(R5900Context *ctx);
    VU1State &vu0StateV1() { return m_vu0.state(); }

private:
    void vu0LoadStateV1(const R5900Context *ctx);
    void vu0RunStateV1(uint32_t address, uint32_t itop, uint32_t top);
    void vu0StoreStateV1(R5900Context *ctx, uint32_t clipBefore);
    uint32_t m_vu0BurstRunsV1 = 0;
    uint32_t m_vu0BurstClipV1 = 0;

public:
    void handleSyscall(uint8_t *rdram, R5900Context *ctx);
    void handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId);
    void handleBreak(uint8_t *rdram, R5900Context *ctx);

    void handleTrap(uint8_t *rdram, R5900Context *ctx);
    void handleTLBR(uint8_t *rdram, R5900Context *ctx);
    void handleTLBWI(uint8_t *rdram, R5900Context *ctx);
    void handleTLBWR(uint8_t *rdram, R5900Context *ctx);
    void handleTLBP(uint8_t *rdram, R5900Context *ctx);
    void clearLLBit(R5900Context *ctx);
    void configureGuestHeap(uint32_t guestBase, uint32_t guestLimit = PS2_RAM_SIZE);
    uint32_t guestMalloc(uint32_t size, uint32_t alignment = 16u);
    uint32_t guestCalloc(uint32_t count, uint32_t size, uint32_t alignment = 16u);
    uint32_t guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment = 16u);
    void guestFree(uint32_t guestAddr);
    uint32_t guestHeapBase() const;
    uint32_t guestHeapEnd() const;
    uint32_t guestHeapLimit() const;

    // Snapshot support for the host-side allocator used by asynchronous guest
    // callbacks (INTC/DMAC handlers). The reservation itself lives in guest
    // RAM, so restoring its high-water mark prevents a fresh runtime from
    // handing that already-live region out again.
    struct AsyncCallbackStackWatermarks
    {
        uint32_t top;
        uint32_t floor;
    };
    AsyncCallbackStackWatermarks asyncCallbackStackWatermarks() const;
    bool restoreAsyncCallbackStackWatermarks(AsyncCallbackStackWatermarks watermarks);

    // SetupThread declares the initial guest-thread stack.  Async callbacks
    // allocate their private stacks from RAM, so this interval must be skipped
    // before any callback can reserve its first frame.
    bool reserveAsyncCallbackStackMainThread(uint32_t stackBase, uint32_t stackLimit);

    uint32_t reserveAsyncCallbackStack(uint32_t size, uint32_t alignment = 16u);
    void dispatchLoop(uint8_t *rdram, R5900Context *ctx);
    struct Gate3PadSampleV1 {
        HostPadState state;
        HostPadCapabilities capabilities;
        float deadzone = -1.0f; // Invalid until explicitly bound; never read from SDL/backend config.
    };
    struct Gate3PadAdmissionV1 {
        uint64_t cycle = 0;
        uint64_t sequence = 0;
        unsigned port = 2; // Invalid until bound.
        Gate3PadSampleV1 sample;
    };
    struct Gate3CdCallbackV1 {
        uint64_t cycle = 0;
        uint64_t sequence = 0;
        uint64_t vblank_start = 0; // Invalid until bound.
    };
    struct Gate3CdReadV1 {
        std::string id;
        uint64_t cycle = 0;
        std::string route;
        uint32_t lbn = 0;
        uint32_t sectors = 0;
        bool iop_destination = false;
        bool success = false;
        std::vector<uint8_t> payload; // User-provided runtime data; never embed in source.
        std::optional<Gate3CdCallbackV1> callback;
    };
    // DeclaredSchedule replays a pre-declared request list. DeterministicNextStart
    // is the product's source rule: synchronous completion at the guest request,
    // payload from the verified user disc, and at most one completion callback
    // at the next effective VBlank-start (pinned CD.cpp pumpCdReadCallbacks).
    enum class Gate3CdPolicyV1 { DeclaredSchedule, DeterministicNextStart };
    struct Gate3CdTraceV1 {
        uint64_t ordinal, cycle, start;
        std::string route;
        uint32_t lbn, sectors;
        bool iop_destination, success;
        uint64_t payload_bytes, payload_fnv1a64; // Comparison digest, not a security hash; 0 when digests are off.
    };
    struct Gate3GuestWorkloadV1 {
        std::array<Gate3PadSampleV1, 2> initial_pad;
        std::vector<Gate3PadAdmissionV1> pad;
        Gate3CdPolicyV1 cd_policy = Gate3CdPolicyV1::DeclaredSchedule;
        std::vector<Gate3CdReadV1> cd;
        // std::less<>: looked up by C string without building a std::string per HLE call (2026-10-05).
        std::map<std::string, uint64_t, std::less<>> hle_costs;
        // Blocks until missing user-owned host data becomes available or throws.
        std::function<void()> wait_for_cd_data;
        // Product storage (bound key `storage persistent_user_card`): the user's
        // memory card persists across sessions. Absent = fresh_zeroed, the
        // qualification default.
        bool persistent_storage = false;
    };
    void gate3ConfigureGuestWorkloadV1(Gate3GuestWorkloadV1 profile);
    // Binds profile + workload from a compiled bound-workload file; returns the
    // source manifest SHA-256 recorded in it.
    std::string gate3LoadBoundWorkloadV1(const std::string& path);
    bool gate3PersistentStorageV1() const
    { return m_gate3GuestWorkloadV1 && m_gate3GuestWorkloadV1->persistent_storage; }
    Gate3PadSampleV1 gate3PadSampleV1(int port);
    void gate3ChargeNamedHleV1(R5900Context* ctx, const char* name);
    // The same charge for kernel syscall `id` (cost name "syscall:<id>"), with the cost looked up once per
    // bound workload: a guest loop makes about 100,000 syscalls in one field at the end of the boot logo.
    void gate3ChargeSyscallHleV1(R5900Context* ctx, uint32_t id);
    void gate3ChargeResolvedHleV1(R5900Context* ctx, uint64_t cost, bool syscall);
    bool gate3ShouldWaitCdHostV1(const char* route, uint32_t lbn, uint32_t sectors, bool resolvable = true);
    void gate3WaitCdHostV1();
    void gate3RecordCdReadV1(const char* route, uint32_t lbn, uint32_t sectors,
                            bool iop_destination, bool success, const std::vector<uint8_t>& payload);
    void gate3SetCdCallbackV1(uint32_t handler, uint32_t gp);
    // Chain routes decide their single completion callback after all segments.
    void gate3CdRouteCompletedV1(const char* route, bool success);
    const std::vector<Gate3CdTraceV1>& gate3CdTraceV1() const { return m_gate3CdTraceV1; }
    // The payload digest is a byte-serial FNV-1a: 16 ms of host time for the 3.4 MB boot read on the Steam Deck
    // (2026-10-05). It only feeds the semantic trace and the in-memory CD trace, so the product turns it off
    // unless the semantic trace is on. Host-only: the guest never sees the digest.
    void gate3SetCdDigestV1(bool on) { m_gate3CdDigestV1 = on; }
    void gate3ServiceAdmissionsV1(R5900Context* ctx);

    void gate3ConfigureTemporalV1(const rrv::guest_time::TemporalProfile& profile);
    rrv::guest_time::TemporalOwner& gate3TemporalV1();
    void gate3HardwareV1();
    void gate3TemporalCheckpointV1(R5900Context* ctx);
    // Gate-4: one out-of-line call per instruction checkpoint: temporal
    // checkpoint, then scheduler checkpoint, each quiet when nothing changed.
    void gate3CheckpointAllV1(R5900Context* ctx);
    // Gate-4 fast-path counters (checkpoints that skipped / ran the full path).
    uint64_t gate3QuietFastV1() const;
    uint64_t gate3QuietFullV1() const;
    void gate3IdleV1();
    void gate3SetWaitContextV1(R5900Context* ctx);
    void gate3AddTimedWaitV1(rrv::guest_time::Scheduler::Ticket ticket, std::function<bool()> ready);
    void gate3ResolveTimedWaitsV1();
    void gate3RunCallbackV1(R5900Context* interrupted, uint32_t handler, uint32_t gp,
                            uint32_t a0, uint32_t a1, uint32_t a2, bool irq = false, uint32_t deferAdmissions = 0);
    void gate3PublishCsrV1();
    bool gate3MmioReadV1(uint32_t address, unsigned width, uint64_t& value);
    // Work that stays inside the armed lazy window (gate3_temporal_runtime.inc).
    bool gate3LazyQuietV1(uint64_t cycle) const;
    bool gate3PlainMmioQuietV1(uint32_t physical) const;
    bool gate3LazyChargeV1(R5900Context* ctx, uint64_t cycles);
    // Session log: plain bus accesses inside the window [0] / with a commit [1], HLE charges [2] / [3].
    static uint64_t gate3LazyWorkCountV1(unsigned index);
    bool gate3MmioWriteV1(uint32_t address, unsigned width, uint64_t value);
    bool gate3AtCutV1() const { return m_gate3TemporalV1 && m_gate3TemporalV1->cut(); }
    void gate3ChargeHleV1(uint64_t cycles);
    bool gate3AdmissionSuppressedV1(uint32_t kind);
    // Guest INTC handler delivery is level-based and never acknowledges
    // INTC_STAT for the guest; the kernel acknowledge policy is unresolved.
    // RR5 registers no INTC handler (static check, 2026-09-24), so the
    // candidate rejects registration/delivery unless a caller binds this
    // unqualified policy explicitly (asset-free tests only).
    void gate3BindUnqualifiedIntcHandlerPolicyV1() { m_gate3IntcHandlerPolicyBoundV1 = true; }
    void gate3RequireIntcHandlerPolicyV1() const {
        if (!m_gate3IntcHandlerPolicyBoundV1)
            throw std::logic_error("Gate3 guest INTC handler: kernel acknowledge policy unresolved");
    }
    // Alarm callbacks are an HLE admission route: they do not model the
    // kernel's Timer3 INTC dispatch (CP0 eligibility, EXL), and every guest
    // context here starts with CP0 Status 0. RR5 never reaches SetAlarm/
    // iSetAlarm (static check, 2026-09-24: no call, pointer or built address
    // of the libkernel stubs or their DI/EI wrappers), so the candidate
    // rejects alarm registration unless a caller binds this unqualified
    // policy explicitly (asset-free tests only).
    void gate3BindUnqualifiedAlarmPolicyV1() { m_gate3AlarmPolicyBoundV1 = true; }
    void gate3RequireAlarmPolicyV1() const {
        if (!m_gate3AlarmPolicyBoundV1)
            throw std::logic_error("Gate3 guest alarm: kernel dispatch eligibility policy unresolved");
    }
    // Qualification diagnostics (env-gated, off by default; see runtime.inc):
    // RRV_GATE3_SEMANTIC_TRACE writes a guest-semantic trace, and
    // RRV_GATE3_HOST_PAUSE / RRV_GATE3_CD_MISSES inject host-only delays.
    void gate3DiagTextV1(const char* text);
    void gate3DiagCdV1(const char* route, uint32_t lbn, uint32_t sectors, bool iop, bool success,
                       uint64_t bytes, uint64_t digest);
    void gate3DiagCdHostV1();
    void gate3DiagSyncVV1(R5900Context* ctx, bool entry);
    // RRV_GATE3_RECORD_PAD=<path>: admit the live host pad at each effective
    // VBlank start and log every change as a replayable full-state admission.
    // RRV_GATE3_LIVE_PAD=1 admits it the same way without a log file.
    void gate3RecordPadV1(uint64_t cycle);
    struct Gate3CutSnapshotV1 {
        uint64_t cycle, effective_starts, gs_services, csr;
        uint32_t intc_stat, intc_mask, pc;
        int context;
        rrv::guest_time::EeTimers timers;
    };
    const std::optional<Gate3CutSnapshotV1>& gate3CutSnapshotV1() const { return m_gate3CutSnapshotV1; }

    rrv::guest_time::Scheduler gate3Scheduler;
    void gate3CheckpointSchedulerV1(int rotate = -1);
    bool shouldPreemptGuestExecution();
    void gate3ConfigureRtcV1(int64_t utc_epoch, int timezone_minutes)
    {
        if (m_gate3RtcV1 || gate3ModeledEeCyclesV1() != 0)
            throw std::logic_error("RTC profile must be bound once before guest execution");
        m_gate3RtcV1 = std::make_unique<rrv::guest_time::GuestRtc>(utc_epoch, timezone_minutes);
        gate3OsdTimezoneMinutesV1() = timezone_minutes;
    }
    // The OSD timezone the guest reads is the workload's RTC offset, never
    // the host timezone database. Process-wide: the OSD config is too.
    static std::optional<int>& gate3OsdTimezoneMinutesV1()
    {
        static std::optional<int> minutes;
        return minutes;
    }
    int64_t gate3RtcLocalSecondsV1() const
    {
        if (!m_gate3RtcV1)
            throw std::logic_error("Gate3 workload RTC identity is not bound");
        return m_gate3RtcV1->LocalSeconds(gate3ModeledEeCyclesV1());
    }
    std::array<uint8_t, 8> gate3ReadRtcV1() const
    {
        if (!m_gate3RtcV1)
            throw std::logic_error("Gate3 workload RTC identity is not bound");
        return m_gate3RtcV1->Read(gate3ModeledEeCyclesV1());
    }


    // Gate-3 temporal accounting API V1. The generator calls the checkpoint
    // before each ordinary instruction/branch; branch plus actual delay slot
    // remain indivisible. No callback is installed by this overlay. All calls
    // require the serialized producer execution domain; this counter is not
    // atomic and must not be read or mutated from an independent host thread.
    using Gate3CheckpointV1 = std::function<void(R5900Context *)>;
    void gate3SetCheckpointV1(Gate3CheckpointV1 callback)
    {
        m_gate3CheckpointV1 = std::move(callback);
        rrv::guest_time::QuietBump();
    }
    // Gate-4 lazy checkpoint (scripts/gate4_lazy_checkpoint_overlay.py): while
    // the window armed by the last slow checkpoint holds, do only what the
    // quiet path would do -- move the owner clock to this instruction's cycle.
    //
    // Called before every guest instruction of the generated code, out of line
    // (189,000 call sites). It is a leaf: everything that calls on, the
    // RRV_GATE4_LAZY_VERIFY check included, is behind the one tail call, so the
    // window path needs no stack frame (Steam Deck profile 2026-10-02: 40% of
    // this function's samples were its three pushes and pops, kept only for
    // the verify call).
    __attribute__((noinline)) void gate3CheckpointV1(R5900Context *ctx)
    {
        const uint64_t cycles = m_gate3ModeledEeCyclesV1;
        auto& lazy = m_gate3LazyV1;
        if (__builtin_expect(ctx == lazy.ctx && cycles < lazy.until && ctx->cop0_status == lazy.cop0 &&
                             rrv::guest_time::quiet_generation.load(std::memory_order_relaxed) == lazy.generation &&
                             !lazy.verify, true)) {
            *lazy.now = cycles;
            return;
        }
        gate3CheckpointSlowV1(ctx);
    }
    __attribute__((noinline)) void gate3CheckpointSlowV1(R5900Context *ctx)
    {
        {
            // The window still holds and RRV_GATE4_LAZY_VERIFY is on: check the
            // predicate, then do what the window path does.
            const uint64_t cycles = m_gate3ModeledEeCyclesV1;
            auto& lazy = m_gate3LazyV1;
            if (__builtin_expect(lazy.verify, false) && ctx == lazy.ctx && cycles < lazy.until &&
                ctx->cop0_status == lazy.cop0 &&
                rrv::guest_time::quiet_generation.load(std::memory_order_relaxed) == lazy.generation) {
                gate3LazyVerifyV1(ctx, cycles);
                *lazy.now = cycles;
                return;
            }
        }
        if (__builtin_expect(m_gate3LazyV1.stats, false)) gate3LazyStatV1(ctx);
        if (__builtin_expect(static_cast<bool>(m_gate3CheckpointV1), false)) {
            gate3CheckpointCallbackV1(ctx);
            return;
        }
        gate3CheckpointAllV1(ctx); // hardware, then scheduler
        gate3ArmLazyV1(ctx);
    }
    void gate3ArmLazyV1(R5900Context *ctx);
    bool gate3LazyPredicateV1(R5900Context *ctx, int context, uint64_t cycles);
    void gate3LazyVerifyV1(R5900Context *ctx, uint64_t cycles);
    void gate3LazyStatV1(R5900Context *ctx);
    // Native hot EE functions (tools/ee-native, scripts/rr5_enhance_overlay.py)
    // account instructions in a local counter. gate4NativeLimitV1 returns the
    // first cycle at which gate3CheckpointV1 would leave the lazy path (0: it
    // would now) and the generation that answer holds for; gate4NativeCommitV1
    // publishes a batch exactly as its checkpoint/begin pairs would have left
    // the runtime: the cycle count, the context's instruction count and, if
    // any fast checkpoint ran, the owner clock at the last one.
    uint64_t gate4NativeLimitV1(R5900Context *ctx, uint64_t &generation) const noexcept
    {
        const auto& lazy = m_gate3LazyV1;
        generation = rrv::guest_time::quiet_generation.load(std::memory_order_relaxed);
        if (ctx != lazy.ctx || ctx->cop0_status != lazy.cop0 || generation != lazy.generation || lazy.verify)
            return 0;
        return lazy.until;
    }
    uint64_t gate4NativeCyclesV1() const noexcept { return m_gate3ModeledEeCyclesV1; }
    void gate4NativeCommitV1(R5900Context *ctx, uint64_t cycles, uint64_t lastCheckpoint, bool anyCheckpoint) noexcept
    {
        ctx->insn_count += cycles - m_gate3ModeledEeCyclesV1;
        m_gate3ModeledEeCyclesV1 = cycles;
        if (anyCheckpoint) *m_gate3LazyV1.now = lastCheckpoint;
    }
    __attribute__((noinline)) void gate3CheckpointCallbackV1(R5900Context *ctx)
    {
        gate3CheckpointAllV1(ctx); // hardware, then scheduler
        if (!m_gate3CheckpointV1) return;
        // The callback may run nested guest work, but must return to this host
        // frame and restore the interrupted guest control coordinates.
        const uint32_t pc = ctx->pc;
        const __m128i sp = ctx->r[29];
        const bool inDelaySlot = ctx->in_delay_slot;
        const uint32_t branchPc = ctx->branch_pc;
        m_gate3CheckpointV1(ctx);
        if (ctx->pc != pc ||
            std::memcmp(&ctx->r[29], &sp, sizeof(sp)) != 0 ||
            ctx->in_delay_slot != inDelaySlot || ctx->branch_pc != branchPc)
            throw std::runtime_error("Gate-3 checkpoint changed guest continuation");
    }
    void gate3BeginInstructionV1(R5900Context *ctx) noexcept
    {
        ++m_gate3ModeledEeCyclesV1; // Runtime-wide V1 modeled EE cycle authority.
        ++ctx->insn_count; // Existing per-context diagnostic instruction count.
    }
    uint64_t gate3ModeledEeCyclesV1() const noexcept
    {
        return m_gate3ModeledEeCyclesV1;
    }
    void gate3ClearModeledEeCyclesV1() noexcept
    {
        m_gate3ModeledEeCyclesV1 = 0;
        rrv::guest_time::QuietBump();
    }
    void requestStop();
    bool isStopRequested() const;
    uint32_t guestExecutionWaiterCountForTesting() const
    {
        return m_guestExecutionWaiters.load(std::memory_order_acquire);
    }

    uint8_t Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint16_t Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint32_t Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint64_t Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    __m128i Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);

    void Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value);
    void Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value);
    void Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value);
    void Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value);
    void Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value);

    // Decides, for every guest load and store of the recompiled code, whether the
    // access needs the slow path (I/O, scratchpad, BIOS, VU memory, TLB segments).
    //
    // The top address nibble answers almost all of them (Steam Deck profile
    // 2026-10-02: the range tests below compile to a move into a vector register,
    // a broadcast and four masked compares at every access, 17% of the hottest
    // native function). Every special range starts at or above physical
    // 0x10000000 and does not wrap (static_asserts below), so:
    //   nibble 0, 8, A   main RAM, direct or through KSEG0/KSEG1: never special
    //   nibble 7         the scratchpad segment: always special
    //   nibble C..F      KSEG2/KSEG3: always special
    // and only the remaining segments run the range tests. The answer is the
    // range tests' for every address.
    static inline bool isSpecialAddress(uint32_t addr)
    {
        static_assert(PS2_IO_BASE >= 0x10000000u && uint64_t(PS2_IO_BASE) + PS2_IO_SIZE <= 0x100000000ull);
        static_assert(PS2_BIOS_BASE >= 0x10000000u && uint64_t(PS2_BIOS_BASE) + PS2_BIOS_SIZE <= 0x100000000ull);
        static_assert(PS2_GS_PRIV_REG_BASE >= 0x10000000u &&
                      uint64_t(PS2_GS_PRIV_REG_BASE) + PS2_GS_PRIV_REG_SIZE <= 0x100000000ull);
        static_assert(PS2_VU0_CODE_BASE >= 0x10000000u && PS2_VU0_CODE_BASE <= PS2_VU1_DATA_BASE);
        static_assert(PS2_SCRATCHPAD_BASE == 0x70000000u);
        const uint32_t segment = addr >> 28;
        if (__builtin_expect(((0x0501u >> segment) & 1u) != 0u, 1))
            return false;
        if (((0xF080u >> segment) & 1u) != 0u)
            return true;

        auto inRange = [](uint32_t value, uint32_t base, uint32_t size) -> bool
        {
            return (value - base) < size;
        };

        auto isPhysicalSpecial = [&](uint32_t physAddr) -> bool
        {
            if (inRange(physAddr, PS2_BIOS_BASE, PS2_BIOS_SIZE))
                return true;
            // Route the ENTIRE scratchpad (SPR) segment 0x70000000-0x7FFFFFFF to the
            // slow path, not just the valid 16KB window. The EE only decodes 16KB of
            // SPR here; addresses past 0x70003FFF are out-of-bounds. If they reach the
            // fast path they get masked by PS2_RAM_MASK (& 0x1FFFFFF) and silently alias
            // onto main RAM — e.g. a runaway display-list walk writing to 0x7031eff0
            // lands on 0x31eff0 and corrupts the game's own jump table. The slow path
            // resolves the valid window to m_scratchpad and drops the rest (no aliasing).
            if (inRange(physAddr, PS2_SCRATCHPAD_BASE, 0x10000000u))
                return true;
            if (inRange(physAddr, PS2_IO_BASE, PS2_IO_SIZE))
                return true;
            if (inRange(physAddr, PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE))
                return true;
            if (physAddr >= PS2_VU0_CODE_BASE && physAddr < (PS2_VU1_DATA_BASE + PS2_VU1_DATA_SIZE))
                return true;
            return false;
        };

        // KSEG2/KSEG3 (TLB mapped)
        if (addr >= 0xC0000000u)
            return true;

        // KSEG0/KSEG1 aliases → physical
        const uint32_t physAddr = (addr >= 0x80000000u) ? (addr & 0x1FFFFFFFu) : addr;
        return isPhysicalSpecial(physAddr);
    }

public:
    inline R5900Context &cpu() { return m_cpuContext; }
    inline const R5900Context &cpu() const { return m_cpuContext; }

    inline PS2Memory &memory() { return m_memory; }
    inline const PS2Memory &memory() const { return m_memory; }

    inline GS &gs() { return m_gs; }
    inline const GS &gs() const { return m_gs; }
    inline GifArbiter &gifArbiter() { return m_gifArbiter; }
    inline const GifArbiter &gifArbiter() const { return m_gifArbiter; }
    inline VU1Interpreter &vu1() { return m_vu1; }
    inline const VU1Interpreter &vu1() const { return m_vu1; }

    inline ps2_iop &iop() { return m_iop; }
    inline const ps2_iop &iop() const { return m_iop; }
    inline PS2AudioBackend &audioBackend() { return m_audioBackend; }
    inline const PS2AudioBackend &audioBackend() const { return m_audioBackend; }
    inline PSPadBackend &padBackend() { return m_padBackend; }
    inline const PSPadBackend &padBackend() const { return m_padBackend; }
    inline rrv::guestoutcome::RuntimeTerminalOutcomeHandle terminalOutcomeHandle() const
    { return m_rrvTerminalOutcome; }

private:
    struct GuestHeapBlock
    {
        uint32_t addr = 0;
        uint32_t size = 0;
        bool free = true;
    };

    static uint32_t alignGuestHeapValue(uint32_t value, uint32_t alignment);
    static bool isGuestHeapAlignmentValid(uint32_t alignment);
    static uint32_t normalizeGuestHeapAlignment(uint32_t alignment);
    uint32_t clampGuestHeapBase(uint32_t guestBase) const;
    uint32_t clampGuestHeapLimit(uint32_t guestLimit) const;
    void resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit);
    void ensureGuestHeapInitializedLocked();
    int32_t findGuestHeapBlockIndexLocked(uint32_t guestAddr) const;
    uint32_t allocateGuestBlockLocked(uint32_t size, uint32_t alignment);
    void freeGuestBlockLocked(uint32_t guestAddr);
    void coalesceGuestHeapLocked();
    void enterGuestExecution();
    void leaveGuestExecution();
    uint32_t releaseGuestExecution();
    void reacquireGuestExecution(uint32_t depth);
    void dispatchGsPacket(GifPathId pathId, const uint8_t *data, uint32_t size,
                          uint32_t vu1Pc, const GifPacketDiagnostic *diagnostic);
    void serviceDirectPresentation();
    void captureDirectPresentationIfRequested(uint64_t fieldIndex);

    void HandleIntegerOverflow(R5900Context *ctx);

    friend class GuestExecutionScope;
    friend class GuestExecutionReleaseScope;

private:
    PS2Memory m_memory;
    GifArbiter m_gifArbiter;
    GS m_gs;
    rrv::gsbackend::Backend m_gsBackend;
    HostPresentationConfig m_hostPresentation{};
#if !defined(PS2X_RRV_FIELD_ONLY)
    // In FullFrame mode PCSX2, not the decode-only local GS, owns the exact
    // packed state. A snapshot taken after boundary N seeds display list N+1.
    std::vector<uint8_t> m_liveFullFrameNextStartVram;
#endif
    ps2_iop m_iop;
    PS2AudioBackend m_audioBackend;
    PSPadBackend m_padBackend;
    VU1Interpreter m_vu1;
    VU1Interpreter m_vu0;
    R5900Context m_cpuContext;
    std::unique_ptr<rrv::guest_time::GuestRtc> m_gate3RtcV1;
    std::unique_ptr<rrv::guest_time::TemporalOwner> m_gate3TemporalV1;
    std::optional<Gate3GuestWorkloadV1> m_gate3GuestWorkloadV1;
    // gate3ChargeNamedHleV1: the cost entry last found for a name's address (the workload is bound once,
    // so the map's nodes stay). The text is compared on every use: a syscall name is a temporary.
    struct Gate3HleCostSlotV1 { const char* name = nullptr; const std::pair<const std::string, uint64_t>* cost = nullptr; };
    std::array<Gate3HleCostSlotV1, 64> m_gate3HleCostCacheV1{};
    std::array<const uint64_t*, 512> m_gate3SyscallCostV1{}; // into hle_costs; cleared when a workload is bound
    std::array<Gate3PadSampleV1, 2> m_gate3AdmittedPadV1{};
    size_t m_gate3NextCdReadV1 = 0;
    size_t m_gate3NextCdCallbackV1 = 0;
    struct Gate3CdReadyV1 {
        std::string id;
        uint64_t start;
        bool issued = false;
        bool admitted = false;
    };
    std::vector<Gate3CdReadyV1> m_gate3CdReadyV1;
    std::vector<Gate3CdTraceV1> m_gate3CdTraceV1;
    bool m_gate3CdDigestV1 = true;
    uint32_t m_gate3CdCallbackV1 = 0;
    uint32_t m_gate3CdCallbackGpV1 = 0;

    struct Gate3TimedWait { rrv::guest_time::Scheduler::Ticket ticket; std::function<bool()> ready; };
    std::vector<Gate3TimedWait> m_gate3TimedWaitsV1;
    bool m_gate3HardwareActiveV1 = false;
    // Gate-4 checkpoint fast path. Armed at the end of a full checkpoint that
    // found nothing to service; any other hardware commit, wait-context,
    // timed-wait or callback change disarms it. The snapshot fields must all
    // still match for the next checkpoint to skip the full path.
    struct Gate3QuietV1 {
        bool armed = false;
        R5900Context* ctx = nullptr; int context = 0;
        uint64_t smode1 = 0, syncv = 0, csr = 0, imr = 0;
        uint32_t stat = 0, mask = 0, cop0 = 0;
        size_t cdReadyBytes = 0; // byte span of m_gate3CdReadyV1 (size x element)
        bool trace = false;
        uint64_t fast = 0, full = 0;
    } m_gate3QuietV1;
    void gate3ArmQuietV1(R5900Context* ctx);
    bool gate3QuietTemporalV1(R5900Context* ctx, int context);
    void gate3QuietTraceV1(R5900Context* ctx, int context);
    size_t gate3CdReadyBytesV1() const {
        return size_t(reinterpret_cast<const char*>(m_gate3CdReadyV1.data() + m_gate3CdReadyV1.size()) -
                      reinterpret_cast<const char*>(m_gate3CdReadyV1.data()));
    }
    void gate3FullTemporalV1(R5900Context* ctx);
    bool m_gate3IntcHandlerPolicyBoundV1 = false;
    bool m_gate3AlarmPolicyBoundV1 = false;
    int m_gate3NextCallbackV1 = -2;
    uint64_t m_gate3GsServiceV1 = 0;
    std::map<int,std::pair<uint32_t,uint64_t>> m_gate3DeferredAdmissionsV1;
    std::optional<Gate3CutSnapshotV1> m_gate3CutSnapshotV1;

    uint64_t m_gate3ModeledEeCyclesV1 = 0;
    // Gate-4 lazy checkpoint window (see gate3CheckpointV1).
    struct Gate4LazyV1 {
        R5900Context* ctx = nullptr;
        uint64_t until = 0, generation = 0;
        uint64_t* now = nullptr;
        uint32_t cop0 = 0;
        bool verify = false, stats = false;
        uint64_t arms = 0;
        uint64_t miss[5] = {}; // RRV_GATE4_LAZY_STATS: unarmed, context, deadline, cop0, generation
    } m_gate3LazyV1;
    Gate3CheckpointV1 m_gate3CheckpointV1;
    rrv::guestoutcome::RuntimeTerminalOutcomeHandle m_rrvTerminalOutcome =
        rrv::guestoutcome::makeRuntimeTerminalOutcome();
    mutable std::recursive_mutex m_guestExecutionMutex;
    mutable std::atomic<uint32_t> m_guestExecutionWaiters{0u};
    mutable std::mutex m_guestHeapMutex;
    mutable std::mutex m_asyncCallbackStackMutex;
    std::vector<GuestHeapBlock> m_guestHeapBlocks;
    uint32_t m_guestHeapBase = 0x00100000u;
    uint32_t m_guestHeapEnd = 0x00100000u;
    uint32_t m_guestHeapLimit = PS2_RAM_SIZE;
    uint32_t m_guestHeapSuggestedBase = 0x00100000u;
    bool m_guestHeapConfigured = false;
    uint32_t m_asyncCallbackStackFloor = 0x01F00000u;
    uint32_t m_asyncCallbackStackTop = PS2_RAM_SIZE;
    uint32_t m_asyncCallbackStackMainBase = 0u;
    uint32_t m_asyncCallbackStackMainLimit = 0u;
    bool m_asyncCallbackStackMainReserved = false;

    std::unordered_map<uint32_t, RecompiledFunction> m_functionTable;
    // Gate 5 (scripts/gate5_dispatch_overlay.py): flat mirror of m_functionTable
    // over guest RAM, one entry per 4-byte pc, pages allocated on registration.
    static constexpr uint32_t kFnPageShift = 12u;
    std::unique_ptr<RecompiledFunction[]> m_fnPages[PS2_RAM_SIZE >> kFnPageShift];
    RecompiledFunction flatFunction(uint32_t address) const
    {
        if (address >= PS2_RAM_SIZE || (address & 3u) != 0u)
            return nullptr;
        const RecompiledFunction *page = m_fnPages[address >> kFnPageShift].get();
        return page ? page[(address & ((1u << kFnPageShift) - 1u)) >> 2] : nullptr;
    }
    std::atomic<bool> m_stopRequested{false};

    // TODO remove this later
    std::atomic<uint32_t> m_debugPc{0};
    std::atomic<uint32_t> m_debugRa{0};
    std::atomic<uint32_t> m_debugSp{0};
    std::atomic<uint32_t> m_debugGp{0};

    struct LoadedModule
    {
        std::string name;
        uint32_t baseAddress;
        size_t size;
        bool active;
    };

    std::vector<LoadedModule> m_loadedModules;
    uint8_t *m_boundRdram = nullptr;
    uint8_t *m_boundGSVram = nullptr;
};

#endif // PS2_RUNTIME_H
