// rrv_guest_state_trace.h — matched phase-6 guest-state oracle (default off).
#ifndef RRV_GUEST_STATE_TRACE_H
#define RRV_GUEST_STATE_TRACE_H

#include "ps2_runtime_macros.h"
#include "rrv_guest_trace_counters.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rrv::guesttrace
{
inline FILE *stream()
{
    static FILE *s_file = []() -> FILE * {
        const char *value = std::getenv("RRV_GUEST_TRACE");
        if (!value || !value[0] || value[0] == '0')
            return nullptr;
        if (std::strcmp(value, "1") == 0 || std::strcmp(value, "-") == 0)
            return stderr;
        FILE *f = std::fopen(value, "w");
        if (!f)
        {
            std::fprintf(stderr, "[guest-trace] cannot open %s\n", value);
            return nullptr;
        }
        std::setvbuf(f, nullptr, _IOLBF, 0);
        return f;
    }();
    return s_file;
}

inline uint32_t read32(const uint8_t *rdram, uint32_t addr)
{
    if (!rdram || addr > 0x01FFFFFCu)
        return 0xFFFFFFFFu;
    uint32_t value;
    std::memcpy(&value, rdram + addr, sizeof(value));
    return value;
}

inline uint16_t read16(const uint8_t *rdram, uint32_t addr)
{
    if (!rdram || addr > 0x01FFFFFEu)
        return 0xFFFFu;
    uint16_t value;
    std::memcpy(&value, rdram + addr, sizeof(value));
    return value;
}

struct Scene
{
    uint32_t phase, script, step;
};

inline Scene scene(const uint8_t *rdram)
{
    Scene s{read32(rdram, 0x334E94u), 0xFFFFFFFFu, 0xFFFFFFFFu};
    const uint32_t cursor = read32(rdram, 0x3348ACu);
    if (cursor && !(cursor & 3u) && cursor <= 0x01FFFFF8u)
    {
        s.script = read32(rdram, cursor);
        s.step = read32(rdram, cursor + 4u);
    }
    return s;
}

inline bool inWindow(uint32_t phase)
{
    static const uint32_t lo = [] { const char *v = std::getenv("RRV_GUEST_TRACE_PHASE_MIN"); return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : 4u; }();
    static const uint32_t hi = [] { const char *v = std::getenv("RRV_GUEST_TRACE_PHASE_MAX"); return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : 7u; }();
    return phase >= lo && phase <= hi;
}

inline void dispatch(uint8_t *rdram, R5900Context *ctx, uint32_t pc)
{
    FILE *f = stream();
    if (!f || !ctx)
        return;
    const Scene sc = scene(rdram);
    if (!inWindow(sc.phase))
        return;
    static uint64_t seq = 0, prevSubmits = 0, prevBytes = 0, prevVsyncs = 0;
    const uint64_t submits = g_gsSubmits.load(std::memory_order_relaxed);
    const uint64_t bytes = g_gsBytes.load(std::memory_order_relaxed);
    const uint64_t vsyncs = g_vsyncs.load(std::memory_order_relaxed);
    const uint32_t selector = read32(rdram, 0x334E04u);
    const uint32_t major = selector >> 16;
    const uint32_t minor = selector & 0xFFFFu;
    const uint32_t table = read32(rdram, 0x002F2A28u + major * 4u);
    const uint32_t resolved = read32(rdram, table + minor * 4u);
    std::fprintf(f,
        "kind=dispatch seq=%llu pc=%08x phase=%u script=%u step=%u flag=%u aux=%u selector=%08x major=%u minor=%u table=%08x target=%08x resolved=%08x "
        "v0=%08x v1=%08x a0=%08x a1=%08x a2=%08x a3=%08x t0=%08x t1=%08x t2=%08x t3=%08x s0=%08x s1=%08x s2=%08x s3=%08x ra=%08x "
        "h_flags=%04x h_334ba0=%08x h_list=%08x h_334d70=%08x h_334d74=%08x h_334d78=%08x h_334d88=%08x h_334d8c=%08x "
        "h_334db8=%08x h_334dbc=%08x h_334e00=%08x h_334e8c=%08x h_334e90=%08x h_334e98=%08x h_state=%08x h_mask=%08x h_334f00=%08x "
        "gs_submits=%llu gs_bytes=%llu vsyncs=%llu d_submits=%llu d_bytes=%llu d_vsyncs=%llu\n",
        static_cast<unsigned long long>(++seq), pc, sc.phase, sc.script, sc.step,
        read32(rdram, 0x334DE4u), read32(rdram, 0x334DC8u), selector, major, minor, table,
        GPR_U32(ctx, 4), resolved, GPR_U32(ctx, 2), GPR_U32(ctx, 3),
        GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7),
        GPR_U32(ctx, 8), GPR_U32(ctx, 9), GPR_U32(ctx, 10), GPR_U32(ctx, 11),
        GPR_U32(ctx, 16), GPR_U32(ctx, 17), GPR_U32(ctx, 18), GPR_U32(ctx, 19), GPR_U32(ctx, 31),
        read16(rdram, 0x3683A0u), read32(rdram, 0x334BA0u), read32(rdram, 0x334BA4u),
        read32(rdram, 0x334D70u), read32(rdram, 0x334D74u), read32(rdram, 0x334D78u),
        read32(rdram, 0x334D88u), read32(rdram, 0x334D8Cu), read32(rdram, 0x334DB8u),
        read32(rdram, 0x334DBCu), read32(rdram, 0x334E00u), read32(rdram, 0x334E8Cu),
        read32(rdram, 0x334E90u), read32(rdram, 0x334E98u), read32(rdram, 0x334EA0u),
        read32(rdram, 0x334EC0u), read32(rdram, 0x334F00u),
        static_cast<unsigned long long>(submits), static_cast<unsigned long long>(bytes),
        static_cast<unsigned long long>(vsyncs), static_cast<unsigned long long>(submits - prevSubmits),
        static_cast<unsigned long long>(bytes - prevBytes), static_cast<unsigned long long>(vsyncs - prevVsyncs));
    prevSubmits = submits; prevBytes = bytes; prevVsyncs = vsyncs;
}

inline void flagWrite(uint8_t *rdram, R5900Context *ctx, uint32_t pc, uint32_t next)
{
    FILE *f = stream();
    if (!f || !ctx)
        return;
    const Scene sc = scene(rdram);
    if (!inWindow(sc.phase))
        return;
    static uint64_t seq = 0;
    std::fprintf(f,
        "kind=flag_write seq=%llu pc=%08x phase=%u script=%u step=%u old=%u new=%u selector=%08x a0=%08x a1=%08x a2=%08x a3=%08x t8=%08x ra=%08x vsyncs=%llu\n",
        static_cast<unsigned long long>(++seq), pc, sc.phase, sc.script, sc.step,
        read32(rdram, 0x334DE4u), next, read32(rdram, 0x334E04u),
        GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7),
        GPR_U32(ctx, 24), GPR_U32(ctx, 31),
        static_cast<unsigned long long>(g_vsyncs.load(std::memory_order_relaxed)));
}

// Phase-7 matrix boundary, byte-for-byte comparable with the scratch PCSX2
// oracle's `kind=mulmatrix` record (pcsx2/x86/ix86-32/iR5900.cpp hook at guest
// 0x2CA250 / 0x23E80C). Same field order, same three bases, same formatting, so
// the two traces diff line-for-line:
//   0x01DE0E30  MulMatrix operand A (projection, built by func_220B88/220A90)
//   0x01DE0CE0  MulMatrix operand B (view; overwritten from 0x01E24E80 by
//               sub_0023E720 before the second call)
//   0x01DE0D20  MulMatrix destination — the phase-7 source matrix
inline void mulMatrix(uint8_t *rdram, R5900Context *ctx, const char *edge)
{
    FILE *f = stream();
    if (!f || !ctx)
        return;
    const Scene sc = scene(rdram);
    if (!inWindow(sc.phase))
        return;
    static uint64_t seq = 0;
    std::fprintf(f, "kind=mulmatrix seq=%llu edge=%s phase=%u script=%u step=%u ra=%08x a0=%08x a1=%08x a2=%08x ",
                 static_cast<unsigned long long>(++seq), edge, sc.phase, sc.script, sc.step,
                 GPR_U32(ctx, 31), GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6));
    for (const uint32_t base : {0x01DE0E30u, 0x01DE0CE0u, 0x01DE0D20u})
    {
        std::fprintf(f, "base%08x=", base);
        for (uint32_t i = 0; i < 16; i++)
            std::fprintf(f, "%s%08x", i ? "/" : "", read32(rdram, base + i * 4u));
        std::fputc(' ', f);
    }
    std::fputc('\n', f);
}

// sceVu0InversMatrix (0x2CA360) boundary, format-matched to the scratch PCSX2
// oracle's hook at guest 0x2CA360 / 0x2CA8C8. `in` carries the source matrix at
// $a1 and the pre-call destination at $a0; `out` carries the 16 words at
// 0x01E24E80 — the sole product of this call on the phase-7 camera path.
// Compare the matrices, not a0/a1 at `out`: on PCSX2 those are post-call guest
// registers, under an HLE arm they are untouched arguments.
inline void inversMatrix(uint8_t *rdram, R5900Context *ctx, const char *edge)
{
    FILE *f = stream();
    if (!f || !ctx)
        return;
    const Scene sc = scene(rdram);
    if (!inWindow(sc.phase))
        return;
    static uint64_t seq = 0;
    const uint32_t dst = GPR_U32(ctx, 4), src = GPR_U32(ctx, 5);
    std::fprintf(f, "kind=inversmatrix seq=%llu edge=%s phase=%u script=%u step=%u ra=%08x a0=%08x a1=%08x ",
                 static_cast<unsigned long long>(++seq), edge, sc.phase, sc.script, sc.step,
                 GPR_U32(ctx, 31), dst, src);
    if (std::strcmp(edge, "in") == 0)
    {
        std::fprintf(f, "src=");
        for (uint32_t i = 0; i < 16; i++)
            std::fprintf(f, "%s%08x", i ? "/" : "", read32(rdram, (src & 0x01FFFFFFu) + i * 4u));
        std::fprintf(f, " predst=");
        for (uint32_t i = 0; i < 16; i++)
            std::fprintf(f, "%s%08x", i ? "/" : "", read32(rdram, (dst & 0x01FFFFFFu) + i * 4u));
    }
    else
    {
        std::fprintf(f, "dst01e24e80=");
        for (uint32_t i = 0; i < 16; i++)
            std::fprintf(f, "%s%08x", i ? "/" : "", read32(rdram, 0x01E24E80u + i * 4u));
    }
    std::fputc('\n', f);
}

// sceVu0Normalize (0x2CA2E0) entry, format-matched to the oracle's hook. Rows 0
// and 2 of the phase-7 camera basis are this function's outputs, so its input
// vector at $a1 is the boundary that decides whether it is the first bad
// function or merely inherits a divergence.
inline void normalize(uint8_t *rdram, R5900Context *ctx, const char *edge)
{
    FILE *f = stream();
    if (!f || !ctx)
        return;
    const Scene sc = scene(rdram);
    if (!inWindow(sc.phase))
        return;
    static uint64_t seq = 0;
    const bool at_entry = (std::strcmp(edge, "in") == 0);
    const uint32_t dst = GPR_U32(ctx, 4), src = GPR_U32(ctx, 5);
    std::fprintf(f, "kind=normalize seq=%llu edge=%s phase=%u script=%u step=%u ra=%08x a0=%08x a1=%08x %s=",
                 static_cast<unsigned long long>(++seq), edge, sc.phase, sc.script, sc.step,
                 GPR_U32(ctx, 31), dst, src, at_entry ? "src" : "out");
    const uint32_t base = (at_entry ? src : dst) & 0x01FFFFFFu;
    for (uint32_t i = 0; i < 4; i++)
        std::fprintf(f, "%s%08x", i ? "/" : "", read32(rdram, base + i * 4u));
    std::fputc('\n', f);
}

// Bounded control for "is sceVu0RotMatrix (0x2CA7C0) on the phase-7 path at
// all?". The decoded chain reaches the rotation leaves through func_220C88,
// not through 0x2CA7C0; this record settles that by measurement.
inline void rotMatrix(uint8_t *rdram, R5900Context *ctx)
{
    FILE *f = stream();
    if (!f || !ctx)
        return;
    const Scene sc = scene(rdram);
    if (!inWindow(sc.phase))
        return;
    static uint64_t seq = 0;
    std::fprintf(f, "kind=rotmatrix seq=%llu phase=%u script=%u step=%u ra=%08x a0=%08x a1=%08x a2=%08x\n",
                 static_cast<unsigned long long>(++seq), sc.phase, sc.script, sc.step,
                 GPR_U32(ctx, 31), GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6));
}

inline void path(uint8_t *rdram, R5900Context *ctx, uint32_t pc)
{
    FILE *f = stream();
    if (!f || !ctx)
        return;
    const Scene sc = scene(rdram);
    if (!inWindow(sc.phase))
        return;
    static uint64_t seq = 0;
    const uint32_t a0 = GPR_U32(ctx, 4);
    std::fprintf(f,
        "kind=path seq=%llu pc=%08x phase=%u script=%u step=%u a0=%08x a1=%08x a2=%08x a3=%08x v0=%08x v1=%08x s0=%08x ra=%08x "
        "p6_index=%08x p6_counter=%08x flush_root=%08x root_word=%08x state=%08x mask=%08x vsyncs=%llu\n",
        static_cast<unsigned long long>(++seq), pc, sc.phase, sc.script, sc.step,
        a0, GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7), GPR_U32(ctx, 2),
        GPR_U32(ctx, 3), GPR_U32(ctx, 16), GPR_U32(ctx, 31), read32(rdram, 0x334F80u),
        read32(rdram, 0x334EC4u), read32(rdram, 0x334DE8u), read32(rdram, a0),
        read32(rdram, 0x334EA0u), read32(rdram, 0x334EC0u),
        static_cast<unsigned long long>(g_vsyncs.load(std::memory_order_relaxed)));
}
} // namespace rrv::guesttrace

#endif
