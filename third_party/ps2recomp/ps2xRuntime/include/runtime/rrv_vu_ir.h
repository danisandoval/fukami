#ifndef RRV_VU_IR_H
#define RRV_VU_IR_H

// rrv_vu_ir.h — decoded VU instruction-slot IR, and the decoder that fills it.
//
// WHY THIS EXISTS
//   Two reasons, in order of immediate value:
//
//   1. The interpreter re-derived every instruction's fields on every
//      execution — ~1.1 M slots per display list, 30 display lists a second,
//      from a microprogram that (measured 2026-08-21) is uploaded ZERO times
//      per second in steady state.  The code is static; decoding it once and
//      caching the result is free correctness-wise and removes the nested
//      upper-special sub-switch from the hot path.
//
//   2. This is the FRONTEND A JIT WOULD USE.  A recompiler's first pass is
//      exactly this: turn 64-bit instruction slots into a typed, dense IR with
//      the operand fields, the destination file, and the side-effect classes
//      broken out.  Keeping that pass separate from execution means the
//      eventual JIT (RR5_TASKS A-7) consumes `SlotIR` and the interpreter stays
//      as its reference implementation and fallback, rather than the two
//      re-deriving the ISA independently and drifting.
//
// WHAT IT IS NOT
//   Not a basic-block or dataflow analysis.  There is no stall model, no flag
//   liveness, no register allocation here.  Those are the JIT's second pass;
//   PCSX2's microVU does them in microVU_Analyze.inl.  This header stops at
//   per-slot decode, which is all the interpreter can use.
//
// VALIDITY
//   A SlotIR carries the raw 64 bits it was decoded from.  A consumer caching
//   decoded slots re-validates with a single 64-bit compare against the live
//   microprogram, so a re-uploaded (MPG) or self-modified program can never be
//   executed from a stale decode.  No epoch counter, no invalidation protocol.

#include <cstdint>

namespace rrv::vu
{

// Dense upper-pipe opcode ids.  Order is arbitrary but stable; the interpreter
// switches on it and a JIT would index its emitter table with it.
enum UpperOp : uint8_t
{
    U_NOP = 0,

    // Write vf[fd]
    U_ADDbc, U_SUBbc, U_MADDbc, U_MSUBbc, U_MAXbc, U_MINIbc, U_MULbc,
    U_MULq, U_MAXi, U_MULi, U_MINIi,
    U_ADDq, U_MADDq, U_ADDi, U_MADDi, U_SUBq, U_MSUBq, U_SUBi, U_MSUBi,
    U_ADD, U_MADD, U_MUL, U_MAX, U_SUB, U_MSUB, U_OPMSUB, U_MINI,

    // Write the accumulator
    U_ADDAbc, U_SUBAbc, U_MADDAbc, U_MSUBAbc, U_MULAbc,
    U_MULAq, U_MULAi,
    U_ADDAq, U_MADDAq, U_ADDAi, U_MADDAi,
    U_SUBAq, U_MSUBAq, U_SUBAi, U_MSUBAi,
    U_ADDA, U_MADDA, U_MULA, U_SUBA, U_MSUBA, U_OPMULA,

    // Write vf[ft]
    U_ITOF0, U_ITOF4, U_ITOF12, U_ITOF15,
    U_FTOI0, U_FTOI4, U_FTOI12, U_FTOI15,
    U_ABS,

    // Writes only the CLIP register
    U_CLIP,

    U_COUNT
};

// Which register file the upper op writes.
enum UpperDst : uint8_t
{
    UD_NONE = 0, // CLIP / NOP
    UD_FD,
    UD_FT,
    UD_ACC
};

// Upper-op properties the interpreter would otherwise re-test per execution.
enum UpperFlags : uint8_t
{
    // The op is an FMAC arithmetic op: its result refreshes the MAC flags and
    // (under RRV_VU_MAC_EXACT) is sanitised by PCSX2's VU_MAC_UPDATE.  This is
    // exactly the set for which PCSX2 calls VU_MACx_UPDATE — MAX/MINI/ITOF/
    // FTOI/ABS/CLIP are excluded.
    UF_MAC = 1u << 0,

    // The op's operands go through the denormal flush when RRV_VU_DENORM is on.
    // NOTE: this reproduces the interpreter's existing exclusion list exactly,
    // INCLUDING upper-special 0x2E (OPMULA), which that list excludes while its
    // comment says "ITOF/FTOI/ABS/CLIP/NOP".  That looks like an oversight, but
    // RRV_VU_DENORM is default-off, so it is dead in every shipping config and
    // is NOT quietly changed here.  If it is ever fixed, it must be measured
    // against PCSX2 like any other semantic change.
    UF_FLUSH = 1u << 1
};

// Lower-pipe classification.  Stage 1 only distinguishes what the interpreter
// can act on; `lop` keeps the raw primary opcode for a future JIT.
enum LowerClass : uint8_t
{
    LC_GENERIC = 0,
    LC_NOP,  // no lower op at all
    LC_LOI   // the I bit is set: the lower word is a float immediate
};

// Upper-word control bits, as the ISA lays them out.
enum UpperBits : uint8_t
{
    UB_T = 1u << 0, // bit 27
    UB_D = 1u << 1, // bit 28
    UB_M = 1u << 2, // bit 29
    UB_E = 1u << 3, // bit 30
    UB_I = 1u << 4  // bit 31
};

// One decoded instruction slot: 20 bytes, trivially copyable.
struct SlotIR
{
    uint32_t lower; // raw words as they sit in microprogram memory —
    uint32_t upper; //   also the cache-validity tag (see VALIDITY above)
    uint8_t uop;    // UpperOp
    uint8_t udst;   // UpperDst
    uint8_t uflags; // UpperFlags
    uint8_t dest;   // xyzw write mask: x=8, y=4, z=2, w=1
    uint8_t fs;
    uint8_t ft;
    uint8_t fd;
    uint8_t bc;     // broadcast lane, 0..3 (x..w)
    uint8_t lclass; // LowerClass
    uint8_t lop;    // raw lower primary opcode, (lower >> 25) & 0x7F
    uint8_t ubits;  // UpperBits
    // 1 once decodeSlot() has filled this entry.  A cache of these starts
    // zeroed, and {lower,upper} = {0,0} is a LEGAL instruction pair (an
    // ADDbc with an empty dest mask, which still writes the MAC flags), so
    // the raw-bits tag alone cannot distinguish "never decoded" from "decoded
    // a pair of zero words".  Consumers must test this before the tag.
    uint8_t valid;
};

namespace detail
{
    struct UpperEntry
    {
        uint8_t op;
        uint8_t dst;
        uint8_t flags;
    };

    static constexpr UpperEntry kMac(uint8_t op, uint8_t dst)
    {
        return UpperEntry{op, dst, static_cast<uint8_t>(UF_MAC | UF_FLUSH)};
    }
    static constexpr UpperEntry kPlain(uint8_t op, uint8_t dst)
    {
        return UpperEntry{op, dst, 0u};
    }

    // Primary upper opcode: bits 5:0 of the upper word.
    inline constexpr UpperEntry kUpper[64] = {
        kMac(U_ADDbc, UD_FD), kMac(U_ADDbc, UD_FD), kMac(U_ADDbc, UD_FD), kMac(U_ADDbc, UD_FD),      // 0x00
        kMac(U_SUBbc, UD_FD), kMac(U_SUBbc, UD_FD), kMac(U_SUBbc, UD_FD), kMac(U_SUBbc, UD_FD),      // 0x04
        kMac(U_MADDbc, UD_FD), kMac(U_MADDbc, UD_FD), kMac(U_MADDbc, UD_FD), kMac(U_MADDbc, UD_FD),  // 0x08
        kMac(U_MSUBbc, UD_FD), kMac(U_MSUBbc, UD_FD), kMac(U_MSUBbc, UD_FD), kMac(U_MSUBbc, UD_FD),  // 0x0C
        kPlain(U_MAXbc, UD_FD), kPlain(U_MAXbc, UD_FD), kPlain(U_MAXbc, UD_FD), kPlain(U_MAXbc, UD_FD),     // 0x10
        kPlain(U_MINIbc, UD_FD), kPlain(U_MINIbc, UD_FD), kPlain(U_MINIbc, UD_FD), kPlain(U_MINIbc, UD_FD), // 0x14
        kMac(U_MULbc, UD_FD), kMac(U_MULbc, UD_FD), kMac(U_MULbc, UD_FD), kMac(U_MULbc, UD_FD),      // 0x18
        kMac(U_MULq, UD_FD),                                                                         // 0x1C
        kPlain(U_MAXi, UD_FD),                                                                       // 0x1D
        kMac(U_MULi, UD_FD),                                                                         // 0x1E
        kPlain(U_MINIi, UD_FD),                                                                      // 0x1F
        kMac(U_ADDq, UD_FD), kMac(U_MADDq, UD_FD), kMac(U_ADDi, UD_FD), kMac(U_MADDi, UD_FD),        // 0x20
        kMac(U_SUBq, UD_FD), kMac(U_MSUBq, UD_FD), kMac(U_SUBi, UD_FD), kMac(U_MSUBi, UD_FD),        // 0x24
        kMac(U_ADD, UD_FD), kMac(U_MADD, UD_FD), kMac(U_MUL, UD_FD), kPlain(U_MAX, UD_FD),           // 0x28
        kMac(U_SUB, UD_FD), kMac(U_MSUB, UD_FD), kMac(U_OPMSUB, UD_FD), kPlain(U_MINI, UD_FD),       // 0x2C
        kPlain(U_NOP, UD_NONE), kPlain(U_NOP, UD_NONE), kPlain(U_NOP, UD_NONE), kPlain(U_NOP, UD_NONE), // 0x30
        kPlain(U_NOP, UD_NONE), kPlain(U_NOP, UD_NONE), kPlain(U_NOP, UD_NONE), kPlain(U_NOP, UD_NONE), // 0x34
        kPlain(U_NOP, UD_NONE), kPlain(U_NOP, UD_NONE), kPlain(U_NOP, UD_NONE), kPlain(U_NOP, UD_NONE), // 0x38
        // 0x3C..0x3F are the upper-special group; the secondary field decides.
        kPlain(U_NOP, UD_NONE), kPlain(U_NOP, UD_NONE), kPlain(U_NOP, UD_NONE), kPlain(U_NOP, UD_NONE)  // 0x3C
    };

    // Upper-special opcode: op2 = (upper & 3) | ((upper >> 4) & 0x7C).
    // The funct field's low 2 bits ARE the broadcast selector, which is why the
    // secondary opcode is assembled this way and not from (upper >> 6) & 0x1F.
    inline constexpr UpperEntry kUpperSpecial[128] = {
        kMac(U_ADDAbc, UD_ACC), kMac(U_ADDAbc, UD_ACC), kMac(U_ADDAbc, UD_ACC), kMac(U_ADDAbc, UD_ACC),     // 0x00
        kMac(U_SUBAbc, UD_ACC), kMac(U_SUBAbc, UD_ACC), kMac(U_SUBAbc, UD_ACC), kMac(U_SUBAbc, UD_ACC),     // 0x04
        kMac(U_MADDAbc, UD_ACC), kMac(U_MADDAbc, UD_ACC), kMac(U_MADDAbc, UD_ACC), kMac(U_MADDAbc, UD_ACC), // 0x08
        kMac(U_MSUBAbc, UD_ACC), kMac(U_MSUBAbc, UD_ACC), kMac(U_MSUBAbc, UD_ACC), kMac(U_MSUBAbc, UD_ACC), // 0x0C
        kPlain(U_ITOF0, UD_FT), kPlain(U_ITOF4, UD_FT), kPlain(U_ITOF12, UD_FT), kPlain(U_ITOF15, UD_FT),   // 0x10
        kPlain(U_FTOI0, UD_FT), kPlain(U_FTOI4, UD_FT), kPlain(U_FTOI12, UD_FT), kPlain(U_FTOI15, UD_FT),   // 0x14
        kMac(U_MULAbc, UD_ACC), kMac(U_MULAbc, UD_ACC), kMac(U_MULAbc, UD_ACC), kMac(U_MULAbc, UD_ACC),     // 0x18
        kMac(U_MULAq, UD_ACC),                                                                              // 0x1C
        kPlain(U_ABS, UD_FT),                                                                               // 0x1D
        kMac(U_MULAi, UD_ACC),                                                                              // 0x1E
        kPlain(U_CLIP, UD_NONE),                                                                            // 0x1F
        kMac(U_ADDAq, UD_ACC), kMac(U_MADDAq, UD_ACC), kMac(U_ADDAi, UD_ACC), kMac(U_MADDAi, UD_ACC),       // 0x20
        kMac(U_SUBAq, UD_ACC), kMac(U_MSUBAq, UD_ACC), kMac(U_SUBAi, UD_ACC), kMac(U_MSUBAi, UD_ACC),       // 0x24
        kMac(U_ADDA, UD_ACC), kMac(U_MADDA, UD_ACC), kMac(U_MULA, UD_ACC), kPlain(U_NOP, UD_NONE),          // 0x28 (0x2B unmapped)
        kMac(U_SUBA, UD_ACC), kMac(U_MSUBA, UD_ACC),                                                        // 0x2C
        // 0x2E OPMULA: kept out of the denormal-flush set to match the
        // interpreter's existing exclusion list — see UF_FLUSH above.
        UpperEntry{U_OPMULA, UD_ACC, UF_MAC},
        kPlain(U_NOP, UD_NONE),                                                                             // 0x2F NOP
        // 0x30..0x7F unmapped -> NOP (zero-initialised: U_NOP / UD_NONE / 0)
    };
} // namespace detail

// Decode one instruction slot.  `lower` and `upper` are the two words exactly
// as they sit in microprogram memory (lower first).
constexpr void decodeSlot(uint32_t lower, uint32_t upper, SlotIR &out)
{
    out.lower = lower;
    out.upper = upper;
    out.dest = static_cast<uint8_t>((upper >> 21) & 0xF);
    out.ft = static_cast<uint8_t>((upper >> 16) & 0x1F);
    out.fs = static_cast<uint8_t>((upper >> 11) & 0x1F);
    out.fd = static_cast<uint8_t>((upper >> 6) & 0x1F);
    out.ubits = static_cast<uint8_t>((upper >> 27) & 0x1F);
    out.lop = static_cast<uint8_t>((lower >> 25) & 0x7F);
    out.valid = 1;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3F);
    if (op >= 0x3C)
    {
        const uint8_t op2 = static_cast<uint8_t>((upper & 0x3) | ((upper >> 4) & 0x7C));
        const detail::UpperEntry &e = detail::kUpperSpecial[op2 & 0x7F];
        out.uop = e.op;
        out.udst = e.dst;
        out.uflags = e.flags;
        out.bc = static_cast<uint8_t>(upper & 0x3);
    }
    else
    {
        const detail::UpperEntry &e = detail::kUpper[op];
        out.uop = e.op;
        out.udst = e.dst;
        out.uflags = e.flags;
        out.bc = static_cast<uint8_t>(op & 0x3);
    }

    if (upper & 0x80000000u)
        out.lclass = LC_LOI;
    else if (lower == 0x00000000u || lower == 0x8000033Cu)
        out.lclass = LC_NOP;
    else
        out.lclass = LC_GENERIC;
}

// Compile-time form for the statically recompiled microcode (RRV_VU_AOT): a
// generated block decodes its constant words with the same decoder.
constexpr SlotIR decodeSlotValue(uint32_t lower, uint32_t upper)
{
    SlotIR s{};
    decodeSlot(lower, upper, s);
    return s;
}

} // namespace rrv::vu

#endif // RRV_VU_IR_H
