// rrv_vu_prog_engine.inc — whole-program native VU microprograms (RRV_VU_PROG).
//
// Included by rrv_vu_aot_engine.inc (so: at the end of ps2_vu1.cpp, after the
// interpreter's op handlers and the compiled-block engine).
//
// WHY. The compiled blocks (Aot::slot) keep every VU register in VU1State and
// are entered one block at a time: each slot loads and stores its operands,
// every block keeps the last MAC word and the last four flag snapshots because
// its successor might read them, and every block is looked up and called
// through a pointer. Measured on the Steam Deck (docs/evidence/
// GATE5_STEAMDECK_VU_EE_2026-10-02.md): the rrv-vu1 thread is the limit in the
// heavy race scenes, 4.3 of its 9.3 ms per field are compiled blocks and 1.7
// more are the dispatch around them, and float arithmetic is a few percent of
// the block time. 93.5% of all VU1 slots of an attract loop are four programs:
// the course strip renderers (entries 0x70 and 0x90 of the two race images).
//
// WHAT. A program is ONE native function for one entry of one microprogram
// image (generated/rr5/vu/rrv_vu_aot_programs.inc, tools/vu-aot/
// vu_prog_gen.py). It runs the same slots in the same order:
//   * the registers the program touches live in a local Regs for the whole run
//     (host registers, once the compiler has taken the struct apart), loaded on
//     entry and stored on exit;
//   * control flow is the function's own: a VU branch is a C++ goto, taken
//     after its delay slot;
//   * the MAC register is lazy: an FMAC op records the value it stored the
//     flags of (Mac::src, Mac::dest) and the word is computed where a flag
//     reader or the exit needs it;
//   * the 4-slot flag visibility is a delay line of four snapshots in Regs,
//     shifted once per slot. Nothing reads most of them, so the compiler
//     removes them; where a reader exists (FCAND in the strip loops, FMAND in
//     the clipping one) it sees the snapshot of four slots earlier, as in
//     VU1Interpreter::run().
// Every op below is the interpreter's formula on the same rrv::simd
// primitives (execUpperFastT, execLowerT with the default configuration,
// which is what the compiled blocks bake); a lower op this file does not
// implement goes through execLowerT itself with the registers written back
// around the call (slotGeneric: XGKICK, the EFU ops).
//
// EXACTNESS. A program leaves the VU1State the interpreter leaves (pc, ebit
// and the last taken branch target included) and the same data memory, and
// kicks the same packets. It hands over to the compiled blocks, with the flag
// ring written back, where it cannot continue: the slot budget (maxCycles)
// would be exceeded, a branch sits in a delay slot, an E bit meets a branch,
// control leaves the image, or an indirect jump goes somewhere the generator
// did not expect. Checked by tests/vu_prog_diff_tests.cpp (every program
// against the interpreter from hostile random state) and, in the game, by
// RRV_VU_PROG_VERIFY=1.

struct VU1Interpreter::Prog
{
    using f32x4 = rrv::simd::f32x4;
    using u32x4 = rrv::simd::u32x4;

    // FLAGS of a slot (tools/vu-aot/vu_prog_gen.py, "Operand analysis"): the
    // lanes this op reads from that operand cannot hold an exponent of 255 on
    // any path to this slot, so the operand clamp is the identity.
    enum : uint32_t
    {
        kCleanVs = 1u,
        kCleanVt = 2u,
        kCleanAcc = 4u
    };

    // Mac::dest of a MAC register that is a plain word (the value on entry).
    static constexpr uint32_t kConcrete = 0xFFFFFFFFu;

    struct Mac
    {
        f32x4 src;     // the raw FMAC result the word is made from
        uint32_t dest; // that op's dest field, or kConcrete
        uint32_t word; // valid when dest == kConcrete
    };
    struct Snap
    {
        Mac mac;
        uint32_t clip;
        uint32_t status;
    };
    struct Regs
    {
        f32x4 vf[32];
        f32x4 acc;
        int32_t vi[16];
        float q;
        float i;
        Mac mac;
        uint32_t clip;
        uint32_t status;
        Snap d[4]; // d[0]: the previous slot's snapshot ... d[3]: four slots ago
        uint32_t branchTarget;
        int32_t top;
        int32_t itop;
    };

    // ---- small vector helpers -------------------------------------------------
    static RRV_SIMD_INLINE float lane(f32x4 v, unsigned k)
    {
        float t[4];
        rrv::simd::storeu(t, v);
        return t[k & 3u];
    }
    static RRV_SIMD_INLINE uint32_t laneBits(f32x4 v, unsigned k)
    {
        uint32_t t[4];
        rrv::simd::storeu_u(t, rrv::simd::as_u(v));
        return t[k & 3u];
    }
    static RRV_SIMD_INLINE f32x4 set4(float x, float y, float z, float w)
    {
        const float t[4] = {x, y, z, w};
        return rrv::simd::loadu(t);
    }
    static RRV_SIMD_INLINE f32x4 vf0()
    {
        return set4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    // The masked register write of applyDestVT / applyDestNoMac.
    static RRV_SIMD_INLINE f32x4 blend(uint32_t dest, f32x4 value, f32x4 old)
    {
        return rrv::simd::select(rrv::simd::dest_mask(static_cast<uint8_t>(dest)), value, old);
    }

    // ---- MAC word and the stored value: macUpdateVT, split --------------------
    static RRV_SIMD_INLINE f32x4 sanitize(f32x4 result)
    {
        using namespace rrv::simd;
        const u32x4 u = as_u(result);
        const u32x4 sign = and_u(u, splat_u(0x80000000u));
        const u32x4 absu = and_u(u, splat_u(0x7FFFFFFFu));
        const u32x4 expo = shr_u<23>(absu);
        const u32x4 isZero = cmpeq_u(absu, splat_u(0u));
        const u32x4 isDen = andnot_u(isZero, cmpeq_u(expo, splat_u(0u)));
        const u32x4 isInf = cmpeq_u(expo, splat_u(0xFFu));
        u32x4 val = select_u(isDen, sign, u);
        val = select_u(isInf, or_u(sign, splat_u(0x7F7FFFFFu)), val);
        return as_f(val);
    }
    static RRV_SIMD_INLINE uint32_t macWord(const Mac &m)
    {
        using namespace rrv::simd;
        if (m.dest == kConcrete)
            return m.word;
        const u32x4 u = as_u(m.src);
        const u32x4 absu = and_u(u, splat_u(0x7FFFFFFFu));
        const u32x4 expo = shr_u<23>(absu);
        const u32x4 isNeg = cmpgt_s(splat_u(0u), u);
        const u32x4 isZero = cmpeq_u(absu, splat_u(0u));
        const u32x4 isDen = andnot_u(isZero, cmpeq_u(expo, splat_u(0u)));
        const u32x4 isInf = cmpeq_u(expo, splat_u(0xFFu));
        static const uint32_t kWz[4] = {0x0008u, 0x0004u, 0x0002u, 0x0001u};
        static const uint32_t kWs[4] = {0x0080u, 0x0040u, 0x0020u, 0x0010u};
        static const uint32_t kWu[4] = {0x0800u, 0x0400u, 0x0200u, 0x0100u};
        static const uint32_t kWo[4] = {0x8000u, 0x4000u, 0x2000u, 0x1000u};
        const u32x4 weighted =
            or_u(or_u(and_u(or_u(isZero, isDen), loadu_u(kWz)), and_u(isNeg, loadu_u(kWs))),
                 or_u(and_u(isDen, loadu_u(kWu)), and_u(isInf, loadu_u(kWo))));
        const uint32_t d = m.dest & 0xFu;
        return reduce_add_u32(weighted) & (d | (d << 4) | (d << 8) | (d << 12));
    }

    // ---- CLIP: clipOp()'s PCSX2-exact form (the program path requires it) ------
    static RRV_SIMD_INLINE uint32_t clipWord(uint32_t clip, f32x4 vs, f32x4 vt)
    {
        int32_t value = static_cast<int32_t>(laneBits(vt, 3));
        value = (value & 0x7F800000) ? (value & 0x7FFFFFFF) : 0x007FFFFF;
        uint32_t flags = 0;
        for (unsigned c = 0; c < 3u; ++c)
        {
            const uint32_t b = laneBits(vs, c);
            if (static_cast<int32_t>(b ^ 0x00000000u) > value)
                flags |= static_cast<uint32_t>(0x01 << (c * 2u));
            if (static_cast<int32_t>(b ^ 0x80000000u) > value)
                flags |= static_cast<uint32_t>(0x02 << (c * 2u));
        }
        return ((clip << 6) | flags) & 0xFFFFFFu;
    }

    // ---- upper pipe: execUpperFastT with the default configuration ------------
    template <uint32_t UPPER, uint32_t FLAGS>
    static RRV_SIMD_INLINE void upper(Regs &r)
    {
        using namespace rrv::simd;
        using namespace rrv::vu;
        static constexpr SlotIR kIr = decodeSlotValue(0u, UPPER);
        if constexpr (kIr.uop != U_NOP)
        {
            f32x4 vs = r.vf[kIr.fs];
            f32x4 vt = r.vf[kIr.ft];
            f32x4 acc = r.acc;
            const f32x4 accRaw = acc;
            if constexpr ((kIr.uflags & UF_FLUSH) != 0u)
            {
                if constexpr ((FLAGS & kCleanVs) == 0u)
                    vs = clampOperandV(vs);
                if constexpr ((FLAGS & kCleanVt) == 0u)
                    vt = clampOperandV(vt);
                if constexpr ((FLAGS & kCleanAcc) == 0u)
                    acc = clampOperandV(acc);
            }
            if constexpr (kIr.uop == U_CLIP)
            {
                r.clip = clipWord(r.clip, vs, vt);
            }
            else
            {
                const f32x4 bc = splat(lane(vt, kIr.bc));
                f32x4 res = vs;
                bool known = true;
                switch (kIr.uop)
                {
                case U_ADDbc:   res = add(vs, bc); break;
                case U_SUBbc:   res = sub(vs, bc); break;
                case U_MADDbc:  res = add(acc, mul(vs, bc)); break;
                case U_MSUBbc:  res = sub(acc, mul(vs, bc)); break;
                case U_MAXbc:   res = max_c(vs, bc); break;
                case U_MINIbc:  res = min_c(vs, bc); break;
                case U_MULbc:   res = mul(vs, bc); break;
                case U_ADDAbc:  res = add(vs, bc); break;
                case U_SUBAbc:  res = sub(vs, bc); break;
                case U_MADDAbc: res = add(acc, mul(vs, bc)); break;
                case U_MSUBAbc: res = sub(acc, mul(vs, bc)); break;
                case U_MULAbc:  res = mul(vs, bc); break;

                case U_MULq:    case U_MULAq:  res = mul(vs, splat(r.q)); break;
                case U_MULi:    case U_MULAi:  res = mul(vs, splat(r.i)); break;
                case U_MAXi:    res = max_c(vs, splat(r.i)); break;
                case U_MINIi:   res = min_c(vs, splat(r.i)); break;
                case U_ADDq:    case U_ADDAq:  res = add(vs, splat(r.q)); break;
                case U_ADDi:    case U_ADDAi:  res = add(vs, splat(r.i)); break;
                case U_SUBq:    case U_SUBAq:  res = sub(vs, splat(r.q)); break;
                case U_SUBi:    case U_SUBAi:  res = sub(vs, splat(r.i)); break;
                case U_MADDq:   case U_MADDAq: res = add(acc, mul(vs, splat(r.q))); break;
                case U_MADDi:   case U_MADDAi: res = add(acc, mul(vs, splat(r.i))); break;
                case U_MSUBq:   case U_MSUBAq: res = sub(acc, mul(vs, splat(r.q))); break;
                case U_MSUBi:   case U_MSUBAi: res = sub(acc, mul(vs, splat(r.i))); break;

                case U_ADD:     case U_ADDA:   res = add(vs, vt); break;
                case U_SUB:     case U_SUBA:   res = sub(vs, vt); break;
                case U_MUL:     case U_MULA:   res = mul(vs, vt); break;
                case U_MADD:    case U_MADDA:  res = add(acc, mul(vs, vt)); break;
                case U_MSUB:    case U_MSUBA:  res = sub(acc, mul(vs, vt)); break;
                case U_MAX:     res = max_c(vs, vt); break;
                case U_MINI:    res = min_c(vs, vt); break;

                // The interpreter's scalar expressions: OPMSUB reads the RAW
                // accumulator and the clamped operands, OPMULA clamps nothing
                // (it is not in the UF_FLUSH set).
                case U_OPMSUB:
                    res = set4(lane(accRaw, 0) - lane(vs, 1) * lane(vt, 2),
                               lane(accRaw, 1) - lane(vs, 2) * lane(vt, 0),
                               lane(accRaw, 2) - lane(vs, 0) * lane(vt, 1), 0.0f);
                    break;
                case U_OPMULA:
                    res = set4(lane(vs, 1) * lane(vt, 2), lane(vs, 2) * lane(vt, 0),
                               lane(vs, 0) * lane(vt, 1), 0.0f);
                    break;

                case U_ITOF0:   res = cvt_s32_to_f32(as_u(vs)); break;
                case U_ITOF4:   res = div(cvt_s32_to_f32(as_u(vs)), splat(16.0f)); break;
                case U_ITOF12:  res = div(cvt_s32_to_f32(as_u(vs)), splat(4096.0f)); break;
                case U_ITOF15:  res = div(cvt_s32_to_f32(as_u(vs)), splat(32768.0f)); break;
                case U_FTOI0:   res = as_f(cvt_f32_to_s32_trunc(vs)); break;
                case U_FTOI4:   res = as_f(cvt_f32_to_s32_trunc(mul(vs, splat(16.0f)))); break;
                case U_FTOI12:  res = as_f(cvt_f32_to_s32_trunc(mul(vs, splat(4096.0f)))); break;
                case U_FTOI15:  res = as_f(cvt_f32_to_s32_trunc(mul(vs, splat(32768.0f)))); break;
                case U_ABS:     res = abs_f(vs); break;

                default: known = false; break; // unmapped: no side effects
                }
                if (known)
                {
                    if constexpr ((kIr.uflags & UF_MAC) != 0u)
                    {
                        r.mac = Mac{res, kIr.dest, 0u};
                        res = sanitize(res);
                    }
                    if constexpr (kIr.udst == UD_ACC)
                        r.acc = blend(kIr.dest, res, r.acc);
                    else if constexpr (kIr.udst == UD_FT)
                        r.vf[kIr.ft] = blend(kIr.dest, res, r.vf[kIr.ft]);
                    else
                        r.vf[kIr.fd] = blend(kIr.dest, res, r.vf[kIr.fd]);
                }
            }
        }
    }

    // ---- lower pipe ------------------------------------------------------------
    static constexpr int16_t imm11(uint32_t i)
    {
        return static_cast<int16_t>(static_cast<int32_t>(static_cast<int32_t>(i << 21) >> 21));
    }
    static constexpr uint32_t op(uint32_t lower) { return (lower >> 25) & 0x7Fu; }
    static constexpr uint32_t funct(uint32_t lower) { return lower & 0x3Fu; }
    static constexpr uint32_t funct2(uint32_t lower) { return (lower & 0x3u) | ((lower >> 4) & 0x7Cu); }
    static constexpr bool isBranch(uint32_t lower)
    {
        const uint32_t o = op(lower);
        return o == 0x20u || o == 0x21u || o == 0x24u || o == 0x25u || o == 0x28u || o == 0x29u ||
               (o >= 0x2Cu && o <= 0x2Fu);
    }
    // The lower ops implemented here; everything else is slotGeneric's.
    static constexpr bool isNative(uint32_t lower)
    {
        const uint32_t o = op(lower);
        switch (o)
        {
        case 0x00: case 0x01: case 0x04: case 0x05: case 0x08: case 0x09:
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x1A: case 0x1B: case 0x1C:
            return true;
        case 0x40:
        {
            const uint32_t f = funct(lower);
            if (f == 0x30u || f == 0x31u || f == 0x32u || f == 0x34u || f == 0x35u)
                return true;
            if (f < 0x3Cu)
                return false;
            switch (funct2(lower))
            {
            case 0x30: case 0x31: case 0x3D: case 0x3C: case 0x3E: case 0x3F:
            case 0x34: case 0x35: case 0x36: case 0x37:
            case 0x38: case 0x39: case 0x3A: case 0x3B:
            case 0x68: case 0x69:
                return true;
            default:
                return false;
            }
        }
        default:
            return false;
        }
    }

    static RRV_SIMD_INLINE uint32_t dataAddr(int32_t reg, uint32_t dataMask)
    {
        return (static_cast<uint32_t>(reg) * 16u) & dataMask;
    }
    static RRV_SIMD_INLINE f32x4 loadQ(const uint8_t *vuData, uint32_t addr)
    {
        float t[4];
        std::memcpy(t, vuData + addr, 16);
        return rrv::simd::loadu(t);
    }
    static RRV_SIMD_INLINE void storeQ(uint8_t *vuData, uint32_t addr, f32x4 v)
    {
        float t[4];
        rrv::simd::storeu(t, v);
        std::memcpy(vuData + addr, t, 16);
    }
    template <uint32_t DEST>
    static RRV_SIMD_INLINE void storeMasked(uint8_t *vuData, uint32_t addr, f32x4 v)
    {
        if constexpr (DEST == 0xFu)
            storeQ(vuData, addr, v);
        else
            storeQ(vuData, addr, blend(DEST, v, loadQ(vuData, addr)));
    }
    template <uint32_t DEST>
    static RRV_SIMD_INLINE void storeWord(uint8_t *vuData, uint32_t addr, uint32_t val)
    {
        if constexpr ((DEST & 0x8u) != 0u)
            std::memcpy(vuData + addr + 0, &val, 4);
        if constexpr ((DEST & 0x4u) != 0u)
            std::memcpy(vuData + addr + 4, &val, 4);
        if constexpr ((DEST & 0x2u) != 0u)
            std::memcpy(vuData + addr + 8, &val, 4);
        if constexpr ((DEST & 0x1u) != 0u)
            std::memcpy(vuData + addr + 12, &val, 4);
    }
    static constexpr unsigned firstLane(uint32_t dest)
    {
        return (dest & 0x8u) ? 0u : (dest & 0x4u) ? 1u : (dest & 0x2u) ? 2u : 3u;
    }

    // A native, non-branch lower op: execLowerT's case, on Regs. `visible` is the
    // snapshot of four slots ago (flag readers); `pc` is this slot's address.
    template <uint32_t LOWER>
    static RRV_SIMD_INLINE void lower(Regs &r, const Snap &visible, uint8_t *vuData, uint32_t dataMask)
    {
        using namespace rrv::simd;
        constexpr uint32_t kOp = op(LOWER);
        constexpr uint32_t it = (LOWER >> 16) & 0x1Fu;
        constexpr uint32_t is = (LOWER >> 11) & 0x1Fu;
        constexpr uint32_t id = (LOWER >> 6) & 0x1Fu;
        constexpr uint32_t dest = (LOWER >> 21) & 0xFu;
        constexpr int32_t imm = imm11(LOWER);
        static_assert(isNative(LOWER) && !isBranch(LOWER), "not a native lower op");

        if constexpr (kOp == 0x00u) // LQ
        {
            const uint32_t addr = dataAddr(r.vi[is & 0xFu] + imm, dataMask);
            r.vf[it] = blend(dest, loadQ(vuData, addr), r.vf[it]);
        }
        else if constexpr (kOp == 0x01u) // SQ: vf[is] to (vi[it] + imm)
        {
            const uint32_t addr = dataAddr(r.vi[it & 0xFu] + imm, dataMask);
            storeMasked<dest>(vuData, addr, r.vf[is]);
        }
        else if constexpr (kOp == 0x04u) // ILW
        {
            const uint32_t addr = dataAddr(r.vi[is & 0xFu] + imm, dataMask);
            uint32_t v;
            std::memcpy(&v, vuData + addr + firstLane(dest) * 4u, 4);
            if constexpr (it != 0u)
                r.vi[it & 0xFu] = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu));
        }
        else if constexpr (kOp == 0x05u) // ISW
        {
            const uint32_t addr = dataAddr(r.vi[is & 0xFu] + imm, dataMask);
            const uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(r.vi[it & 0xFu] & 0xFFFF));
            storeWord<dest>(vuData, addr, val);
        }
        else if constexpr (kOp == 0x08u || kOp == 0x09u) // IADDIU / ISUBIU
        {
            constexpr int16_t imm15 =
                static_cast<int16_t>(static_cast<int16_t>(LOWER & 0x7FFu) | ((LOWER >> 10) & 0x7800u));
            if constexpr (it != 0u)
                r.vi[it & 0xFu] = (kOp == 0x08u) ? static_cast<int16_t>(r.vi[is & 0xFu] + imm15)
                                                 : static_cast<int16_t>(r.vi[is & 0xFu] - imm15);
        }
        else if constexpr (kOp == 0x10u) // FCEQ
            r.vi[1] = ((visible.clip & 0xFFFFFFu) == (LOWER & 0xFFFFFFu)) ? 1 : 0;
        else if constexpr (kOp == 0x11u) // FCSET
            r.clip = LOWER & 0xFFFFFFu;
        else if constexpr (kOp == 0x12u) // FCAND
            r.vi[1] = ((visible.clip & (LOWER & 0xFFFFFFu)) != 0u) ? 1 : 0;
        else if constexpr (kOp == 0x13u) // FCOR
            r.vi[1] = ((visible.clip | (LOWER & 0xFFFFFFu)) == 0xFFFFFFu) ? 1 : 0;
        else if constexpr (kOp == 0x14u) // FSEQ
            r.vi[1] = ((visible.status & 0xFFFu) == (LOWER & 0xFFFu)) ? 1 : 0;
        else if constexpr (kOp == 0x15u) // FSSET
            r.status = (LOWER >> 6) & 0xFC0u;
        else if constexpr (kOp == 0x16u) // FSAND
            r.vi[1] = static_cast<int32_t>(visible.status & (LOWER & 0xFFFu));
        else if constexpr (kOp == 0x17u) // FSOR
            r.vi[1] = ((visible.status | (LOWER & 0xFFFu)) == 0xFFFu) ? 1 : 0;
        else if constexpr (kOp == 0x18u) // FMEQ
        {
            const uint32_t macVal = macWord(visible.mac);
            if constexpr (it != 0u)
                r.vi[it & 0xFu] =
                    ((macVal & 0xFFFFu) == static_cast<uint32_t>(static_cast<uint16_t>(r.vi[is & 0xFu]))) ? 1 : 0;
        }
        else if constexpr (kOp == 0x1Au) // FMAND
        {
            const uint32_t macVal = macWord(visible.mac);
            if constexpr (it != 0u)
                r.vi[it & 0xFu] =
                    static_cast<int32_t>(macVal & static_cast<uint32_t>(static_cast<uint16_t>(r.vi[is & 0xFu])));
        }
        else if constexpr (kOp == 0x1Bu) // FMOR
        {
            const uint32_t macVal = macWord(visible.mac);
            if constexpr (it != 0u)
                r.vi[it & 0xFu] =
                    static_cast<int32_t>(macVal | static_cast<uint32_t>(static_cast<uint16_t>(r.vi[is & 0xFu])));
        }
        else if constexpr (kOp == 0x1Cu) // FCGET
        {
            if constexpr (it != 0u)
                r.vi[it & 0xFu] = static_cast<int32_t>(visible.clip & 0xFFFu);
        }
        else // 0x40: lower special
        {
            constexpr uint32_t kFunct = funct(LOWER);
            constexpr uint32_t kF2 = funct2(LOWER);
            if constexpr (kFunct == 0x30u) // IADD
            {
                if constexpr (id != 0u)
                    r.vi[id & 0xFu] = static_cast<int16_t>(r.vi[is & 0xFu] + r.vi[it & 0xFu]);
            }
            else if constexpr (kFunct == 0x31u) // ISUB
            {
                if constexpr (id != 0u)
                    r.vi[id & 0xFu] = static_cast<int16_t>(r.vi[is & 0xFu] - r.vi[it & 0xFu]);
            }
            else if constexpr (kFunct == 0x32u) // IADDI
            {
                constexpr int16_t imm5 =
                    static_cast<int16_t>(static_cast<int32_t>(((LOWER >> 6) & 0x1Fu) << 27) >> 27);
                if constexpr (it != 0u)
                    r.vi[it & 0xFu] = static_cast<int16_t>(r.vi[is & 0xFu] + imm5);
            }
            else if constexpr (kFunct == 0x34u) // IAND
            {
                if constexpr (id != 0u)
                    r.vi[id & 0xFu] = r.vi[is & 0xFu] & r.vi[it & 0xFu];
            }
            else if constexpr (kFunct == 0x35u) // IOR
            {
                if constexpr (id != 0u)
                    r.vi[id & 0xFu] = r.vi[is & 0xFu] | r.vi[it & 0xFu];
            }
            else if constexpr (kF2 == 0x30u) // MOVE
                r.vf[it] = blend(dest, r.vf[is], r.vf[it]);
            else if constexpr (kF2 == 0x31u) // MR32
            {
                const f32x4 s = r.vf[is];
                r.vf[it] = blend(dest, set4(lane(s, 1), lane(s, 2), lane(s, 3), lane(s, 0)), r.vf[it]);
            }
            else if constexpr (kF2 == 0x3Du) // MFIR
            {
                const int32_t val = static_cast<int32_t>(static_cast<int16_t>(r.vi[is & 0xFu] & 0xFFFF));
                r.vf[it] = blend(dest, as_f(splat_u(static_cast<uint32_t>(val))), r.vf[it]);
            }
            else if constexpr (kF2 == 0x3Cu) // MTIR
            {
                const uint32_t fval = laneBits(r.vf[is], (LOWER >> 21) & 0x3u);
                if constexpr (it != 0u)
                    r.vi[it & 0xFu] = static_cast<int32_t>(static_cast<int16_t>(fval & 0xFFFFu));
            }
            else if constexpr (kF2 == 0x3Eu) // ILWR
            {
                const uint32_t addr = dataAddr(static_cast<uint16_t>(r.vi[is & 0xFu]), dataMask);
                uint32_t v;
                std::memcpy(&v, vuData + addr + firstLane(dest) * 4u, 4);
                if constexpr (it != 0u)
                    r.vi[it & 0xFu] = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu));
            }
            else if constexpr (kF2 == 0x3Fu) // ISWR
            {
                const uint32_t addr = dataAddr(static_cast<uint16_t>(r.vi[is & 0xFu]), dataMask);
                const uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(r.vi[it & 0xFu] & 0xFFFF));
                storeWord<dest>(vuData, addr, val);
            }
            else if constexpr (kF2 == 0x34u) // LQI
            {
                const uint32_t addr = dataAddr(static_cast<uint16_t>(r.vi[is & 0xFu]), dataMask);
                r.vf[it] = blend(dest, loadQ(vuData, addr), r.vf[it]);
                if constexpr (is != 0u)
                    r.vi[is & 0xFu] = static_cast<int16_t>(r.vi[is & 0xFu] + 1);
            }
            else if constexpr (kF2 == 0x35u) // SQI
            {
                const uint32_t addr = dataAddr(static_cast<uint16_t>(r.vi[it & 0xFu]), dataMask);
                storeMasked<dest>(vuData, addr, r.vf[is]);
                if constexpr (it != 0u)
                    r.vi[it & 0xFu] = static_cast<int16_t>(r.vi[it & 0xFu] + 1);
            }
            else if constexpr (kF2 == 0x36u) // LQD
            {
                if constexpr (is != 0u)
                    r.vi[is & 0xFu] = static_cast<int16_t>(r.vi[is & 0xFu] - 1);
                const uint32_t addr = dataAddr(static_cast<uint16_t>(r.vi[is & 0xFu]), dataMask);
                r.vf[it] = blend(dest, loadQ(vuData, addr), r.vf[it]);
            }
            else if constexpr (kF2 == 0x37u) // SQD
            {
                if constexpr (it != 0u)
                    r.vi[it & 0xFu] = static_cast<int16_t>(r.vi[it & 0xFu] - 1);
                const uint32_t addr = dataAddr(static_cast<uint16_t>(r.vi[it & 0xFu]), dataMask);
                storeMasked<dest>(vuData, addr, r.vf[is]);
            }
            else if constexpr (kF2 == 0x38u) // DIV (the default rule; RRV_VU_DIV_EXACT is not a program's)
            {
                const float num = lane(r.vf[is], (LOWER >> 21) & 0x3u);
                const float den = lane(r.vf[it], (LOWER >> 23) & 0x3u);
                r.q = den != 0.0f ? num / den
                                  : ((num >= 0.0f) ? std::numeric_limits<float>::max()
                                                   : -std::numeric_limits<float>::max());
            }
            else if constexpr (kF2 == 0x39u) // SQRT
                r.q = std::sqrt(std::fabs(lane(r.vf[it], (LOWER >> 23) & 0x3u)));
            else if constexpr (kF2 == 0x3Au) // RSQRT
            {
                const float num = lane(r.vf[is], (LOWER >> 21) & 0x3u);
                const float den = std::sqrt(std::fabs(lane(r.vf[it], (LOWER >> 23) & 0x3u)));
                r.q = den != 0.0f ? num / den : std::numeric_limits<float>::max();
            }
            else if constexpr (kF2 == 0x3Bu) // WAITQ: nothing is staged without RRV_VU_Q_LATENCY
            {
            }
            else if constexpr (kF2 == 0x68u) // XTOP
            {
                if constexpr (it != 0u)
                    r.vi[it & 0xFu] = r.top;
            }
            else // 0x69 XITOP
            {
                if constexpr (it != 0u)
                    r.vi[it & 0xFu] = r.itop;
            }
        }
    }

    // A branch's decision, taken in its own slot (run() transfers after the delay
    // slot). Returns whether it is taken; BAL/JALR write their link register.
    template <uint32_t LOWER>
    static RRV_SIMD_INLINE bool branch(Regs &r, uint32_t pc)
    {
        constexpr uint32_t kOp = op(LOWER);
        constexpr uint32_t it = (LOWER >> 16) & 0x1Fu;
        constexpr uint32_t is = (LOWER >> 11) & 0x1Fu;
        static_assert(isBranch(LOWER), "not a branch");
        if constexpr (kOp == 0x21u || kOp == 0x25u) // BAL, JALR
        {
            if constexpr (it != 0u)
                r.vi[it & 0xFu] = static_cast<int32_t>((pc + 16u) / 8u);
            return true;
        }
        else if constexpr (kOp == 0x20u || kOp == 0x24u) // B, JR
            return true;
        else if constexpr (kOp == 0x28u)
            return static_cast<int16_t>(r.vi[is & 0xFu]) == static_cast<int16_t>(r.vi[it & 0xFu]);
        else if constexpr (kOp == 0x29u)
            return static_cast<int16_t>(r.vi[is & 0xFu]) != static_cast<int16_t>(r.vi[it & 0xFu]);
        else if constexpr (kOp == 0x2Cu)
            return static_cast<int16_t>(r.vi[is & 0xFu]) < 0;
        else if constexpr (kOp == 0x2Du)
            return static_cast<int16_t>(r.vi[is & 0xFu]) > 0;
        else if constexpr (kOp == 0x2Eu)
            return static_cast<int16_t>(r.vi[is & 0xFu]) <= 0;
        else
            return static_cast<int16_t>(r.vi[is & 0xFu]) >= 0;
    }
    // JR / JALR: the target, read before the link register is written (JALR
    // with is == it jumps to the old value, as execLowerT computes it).
    template <uint32_t LOWER>
    static RRV_SIMD_INLINE uint32_t jumpTarget(const Regs &r)
    {
        constexpr uint32_t is = (LOWER >> 11) & 0x1Fu;
        return (static_cast<uint32_t>(static_cast<uint16_t>(r.vi[is & 0xFu])) * 8u) & 0x3FFFu;
    }

    // ---- one slot: Aot::slot / run()'s per-slot sequence on Regs --------------
    template <uint32_t UPPER, uint32_t FLAGS>
    static RRV_SIMD_INLINE Snap head(Regs &r, uint32_t lowerWord)
    {
        const Snap visible = r.d[3];
        if constexpr ((UPPER & 0x80000000u) != 0u) // LOI
            std::memcpy(&r.i, &lowerWord, 4);
        upper<UPPER, FLAGS>(r);
        r.d[3] = r.d[2];
        r.d[2] = r.d[1];
        r.d[1] = r.d[0];
        r.d[0] = Snap{r.mac, r.clip, r.status};
        return visible;
    }
    static RRV_SIMD_INLINE void tail(Regs &r)
    {
        r.vf[0] = vf0();
        r.vi[0] = 0;
    }
    static constexpr bool hasLower(uint32_t lower, uint32_t upperWord)
    {
        return (upperWord & 0x80000000u) == 0u && lower != 0x00000000u && lower != 0x8000033Cu;
    }

    template <uint32_t LOWER, uint32_t UPPER, uint32_t FLAGS>
    static RRV_SIMD_INLINE void slot(Regs &r, uint8_t *vuData, uint32_t dataMask)
    {
        const Snap visible = head<UPPER, FLAGS>(r, LOWER);
        if constexpr (hasLower(LOWER, UPPER))
            lower<LOWER>(r, visible, vuData, dataMask);
        else
            (void)visible;
        tail(r);
    }

    // A slot whose lower op is a branch. Returns whether the branch is taken.
    template <uint32_t LOWER, uint32_t UPPER, uint32_t FLAGS>
    static RRV_SIMD_INLINE bool slotBranch(Regs &r, uint32_t pc)
    {
        (void)head<UPPER, FLAGS>(r, LOWER);
        const bool taken = branch<LOWER>(r, pc);
        if (taken && (op(LOWER) != 0x24u && op(LOWER) != 0x25u))
            r.branchTarget = (pc + 8u + static_cast<uint32_t>(static_cast<int32_t>(imm11(LOWER)) * 8)) & 0x3FFFu;
        tail(r);
        return taken;
    }
    // JR / JALR: returns the target address.
    template <uint32_t LOWER, uint32_t UPPER, uint32_t FLAGS>
    static RRV_SIMD_INLINE uint32_t slotJump(Regs &r, uint32_t pc)
    {
        (void)head<UPPER, FLAGS>(r, LOWER);
        const uint32_t target = jumpTarget<LOWER>(r);
        (void)branch<LOWER>(r, pc);
        r.branchTarget = target;
        tail(r);
        return target;
    }

    // ---- entry / exit / registers written back around a generic op ------------
    template <uint32_t MASK, unsigned K>
    static RRV_SIMD_INLINE void loadVf(Regs &r, const VU1State &s)
    {
        if constexpr (((MASK >> K) & 1u) != 0u)
            r.vf[K] = rrv::simd::loadu(s.vf[K]);
        if constexpr (K + 1u < 32u)
            loadVf<MASK, K + 1u>(r, s);
    }
    template <uint32_t MASK, unsigned K>
    static RRV_SIMD_INLINE void storeVf(VU1State &s, const Regs &r)
    {
        if constexpr (((MASK >> K) & 1u) != 0u)
            rrv::simd::storeu(s.vf[K], r.vf[K]);
        if constexpr (K + 1u < 32u)
            storeVf<MASK, K + 1u>(s, r);
    }
    template <uint32_t MASK, unsigned K>
    static RRV_SIMD_INLINE void loadVi(Regs &r, const VU1State &s)
    {
        if constexpr (((MASK >> K) & 1u) != 0u)
            r.vi[K] = s.vi[K];
        if constexpr (K + 1u < 16u)
            loadVi<MASK, K + 1u>(r, s);
    }
    template <uint32_t MASK, unsigned K>
    static RRV_SIMD_INLINE void storeVi(VU1State &s, const Regs &r)
    {
        if constexpr (((MASK >> K) & 1u) != 0u)
            s.vi[K] = r.vi[K];
        if constexpr (K + 1u < 16u)
            storeVi<MASK, K + 1u>(s, r);
    }

    // VF / VI: the registers the program's native ops name (bit k = register k).
    template <uint32_t VF, uint32_t VI>
    static RRV_SIMD_INLINE void enter(Regs &r, const VU1Interpreter &vu)
    {
        const VU1State &s = vu.m_state;
        loadVf<VF, 0u>(r, s);
        loadVi<VI, 0u>(r, s);
        r.acc = rrv::simd::loadu(s.acc);
        r.q = s.q;
        r.i = s.i;
        r.mac = Mac{rrv::simd::zero_f(), kConcrete, s.mac};
        r.clip = s.clip;
        r.status = s.status;
        // aotLean() has just seeded the ring with the entry flags.
        const Snap seed{r.mac, r.clip, r.status};
        r.d[0] = seed;
        r.d[1] = seed;
        r.d[2] = seed;
        r.d[3] = seed;
        r.branchTarget = s.branchTarget;
        r.top = static_cast<int32_t>(s.top);
        r.itop = static_cast<int32_t>(s.itop);
    }

    template <uint32_t VF, uint32_t VI>
    static RRV_SIMD_INLINE void storeRegs(VU1Interpreter &vu, const Regs &r)
    {
        VU1State &s = vu.m_state;
        storeVf<VF, 0u>(s, r);
        storeVi<VI, 0u>(s, r);
        rrv::simd::storeu(s.acc, r.acc);
        s.q = r.q;
        s.i = r.i;
        s.mac = macWord(r.mac);
        s.clip = r.clip;
        s.status = r.status;
        s.branchTarget = r.branchTarget;
    }

    // The end of a run. `ended`: the program ended (E bit); otherwise the
    // compiled blocks continue at `pc`, so the flag ring is theirs again.
    template <uint32_t VF, uint32_t VI>
    static RRV_SIMD_INLINE void leave(VU1Interpreter &vu, const Regs &r, uint32_t pc, bool ended)
    {
        storeRegs<VF, VI>(vu, r);
        VU1State &s = vu.m_state;
        s.pc = pc;
        s.ebit = ended;
        s.branchPending = false;
        // The next slot reads ring[pos] as "four slots ago".
        vu.m_flagRing[0] = FlagSnapshot{macWord(r.d[3].mac), r.d[3].clip, r.d[3].status};
        vu.m_flagRing[1] = FlagSnapshot{macWord(r.d[2].mac), r.d[2].clip, r.d[2].status};
        vu.m_flagRing[2] = FlagSnapshot{macWord(r.d[1].mac), r.d[1].clip, r.d[1].status};
        vu.m_flagRing[3] = FlagSnapshot{macWord(r.d[0].mac), r.d[0].clip, r.d[0].status};
        vu.m_flagRingPos = 0;
    }

    // run()'s fall-through pc, wrap included.
    static RRV_SIMD_INLINE uint32_t nextPc(const VU1Interpreter &vu, uint32_t pc)
    {
        const uint32_t next = pc + 8u;
        return next >= vu.m_aotCodeSize ? 0u : next;
    }

    // A slot whose lower op is not implemented above: the upper op runs here,
    // the lower op in execLowerT on VU1State, with the registers written back
    // before and read again after.
    template <uint32_t LOWER, uint32_t UPPER, uint32_t FLAGS, uint32_t VF, uint32_t VI>
    static RRV_SIMD_INLINE void slotGeneric(VU1Interpreter &vu, Regs &r, uint32_t pc)
    {
        static_assert(hasLower(LOWER, UPPER) && !isNative(LOWER) && !isBranch(LOWER), "not a generic lower op");
        const Snap visible = head<UPPER, FLAGS>(r, LOWER);
        storeRegs<VF, VI>(vu, r);
        vu.m_state.pc = pc;
        vu.m_flagVisible = FlagSnapshot{macWord(visible.mac), visible.clip, visible.status};
        constexpr int kOp = static_cast<int>(op(LOWER));
        constexpr int kFunct = kOp == 0x40 ? static_cast<int>(funct(LOWER)) : -1;
        constexpr int kF2 = (kOp == 0x40 && kFunct >= 0x3C) ? static_cast<int>(funct2(LOWER)) : -1;
        vu.template execLowerT<kOp, kFunct, kF2>(LOWER, vu.m_curData, vu.m_curDataSize, *vu.m_curGs,
                                                 vu.m_curMemory, UPPER);
        const VU1State &s = vu.m_state;
        loadVf<VF, 0u>(r, s);
        loadVi<VI, 0u>(r, s);
        r.q = s.q;
        r.clip = s.clip;
        r.status = s.status;
        tail(r);
    }
};

namespace
{
#if __has_include("rrv_vu_aot_programs.inc")
#include "rrv_vu_aot_programs.inc"
#else
// No programs in this build: every lookup misses and the compiled blocks run.
const VU1Interpreter::ProgEntry *const kVuProgs = nullptr;
constexpr size_t kVuProgCount = 0;
constexpr const char kVuProgCatalogId[] = "none";
#endif
} // namespace

void VU1Interpreter::progEnsure()
{
    if (m_progInit)
        return;
    m_progInit = true;
    // RRV_VU_PROG: 1 / 0 select it; unset, it follows the `native_code` setting
    // (RRV_RR5_NATIVE_HOT), which is how the product enables host-only native
    // code. VU1 only: VU0 micro mode runs a few dozen slots per call from EE
    // code and keeps the compiled blocks.
    bool on = vuAotEnvOn("RRV_RR5_NATIVE_HOT", false);
    if (const char *v = std::getenv("RRV_VU_PROG"); v && v[0])
        on = !(v[0] == '0' && v[1] == '\0');
    m_progVerify = vuAotEnvOn("RRV_VU_PROG_VERIFY", false);
    m_progEnabled = (on || m_progVerify) && !m_isVu0 && kVuProgCount != 0u && vuClipExactEnabled();
    if (!m_isVu0)
        std::fprintf(stderr, "[vu-prog] %s for vu1: catalogue %s, %zu programs%s\n",
                     m_progEnabled ? "enabled" : "disabled", kVuProgCatalogId, kVuProgCount,
                     m_progVerify ? ", RRV_VU_PROG_VERIFY=1 (differential gate)" : "");
}

// RRV_VU_PROG_CAPTURE=<dir> (developer tool; the directory must be under local/:
// the files hold game data): every RRV_VU_PROG_CAPTURE_EVERY-th VU1 invocation
// (default 20011), at most 600, is written out whole (VU1State, micro memory,
// data memory) for tests/vu_prog_diff_tests.cpp --bench.
static void vuProgCapture(const VU1State &state, const uint8_t *vuCode, uint32_t codeSize,
                          const uint8_t *vuData, uint32_t dataSize, uint32_t startPC, uint32_t itop,
                          uint32_t top)
{
    static const char *const s_dir = [] {
        const char *d = std::getenv("RRV_VU_PROG_CAPTURE");
        return (d && d[0]) ? d : nullptr;
    }();
    if (!s_dir)
        return;
    static const uint64_t s_every = [] {
        const char *v = std::getenv("RRV_VU_PROG_CAPTURE_EVERY");
        const long n = (v && v[0]) ? std::atol(v) : 20011L;
        return static_cast<uint64_t>(n > 0 ? n : 20011L);
    }();
    static uint64_t s_calls = 0;
    static uint32_t s_written = 0;
    if ((++s_calls % s_every) != 0u || s_written >= 600u)
        return;
    char path[1100];
    std::snprintf(path, sizeof(path), "%s/%05u-%04x.vucap", s_dir, s_written, startPC);
    if (FILE *f = std::fopen(path, "wb"))
    {
        const uint32_t header[4] = {0x50564343u, startPC, itop, top};
        std::fwrite(header, 1, sizeof(header), f);
        std::fwrite(&state, 1, sizeof(state), f);
        std::fwrite(vuCode, 1, codeSize, f);
        std::fwrite(vuData, 1, dataSize, f);
        std::fclose(f);
        ++s_written;
    }
}

void VU1Interpreter::progReport(const char *why)
{
    if (m_isVu0 || !m_progInit || (m_progCalls | m_progNoProgram) == 0u)
        return;
    std::fprintf(stderr,
                 "[vu-prog] vu1 %s: %llu invocations native (%llu slots), %llu handed over to blocks, "
                 "%llu without a program\n",
                 why, (unsigned long long)m_progCalls, (unsigned long long)m_progSlots,
                 (unsigned long long)m_progHandOvers, (unsigned long long)m_progNoProgram);
}

// The program for an invocation at `pc`, or nullptr. Valid for one micro-memory
// generation per pc (g_rrvVuCodeGen, bumped by every write to micro memory), so
// the common case is one compare.
const VU1Interpreter::ProgEntry *VU1Interpreter::progLookup(uint32_t pc, const uint8_t *vuCode,
                                                            uint32_t codeSize)
{
    const uint32_t idx = pc >> 3;
    if ((pc & 7u) != 0u || idx >= kIrSlots)
        return nullptr;
    const uint32_t gen = g_rrvVuCodeGen[m_isVu0 ? 0 : 1].load(std::memory_order_relaxed);
    if (m_progByPcGen[idx] == gen)
        return m_progByPc[idx];
    const ProgEntry *found = nullptr;
    if (pc + 8u <= codeSize)
    {
        uint64_t first;
        std::memcpy(&first, vuCode + pc, sizeof(first));
        for (size_t k = 0; k < kVuProgCount && !found; ++k)
        {
            const ProgEntry &e = kVuProgs[k];
            if (e.first != first || e.entry > pc)
                continue;
            const uint32_t base = pc - e.entry;
            bool same = true;
            for (uint32_t n = 0; n < e.rangeCount && same; ++n)
            {
                const ProgRange &g = e.ranges[n];
                same = base + g.offset + g.slots * 8u <= codeSize &&
                       std::memcmp(vuCode + base + g.offset, e.raw + (g.offset >> 3), g.slots * 8u) == 0;
            }
            if (same)
                found = &e;
        }
    }
    m_progByPc[idx] = found;
    m_progByPcGen[idx] = gen;
    return found;
}

// RRV_VU_PROG_VERIFY: the invocation runs through the compiled blocks first
// (the results the game keeps, real side effects), then again from the same
// snapshot through the program with its GIF output suppressed. VU1State, the
// data memory and the XGKICK sequence (pc, address, hash of the data memory at
// the kick) must be identical. Returns whether the invocation ended.
bool VU1Interpreter::progVerified(const ProgEntry *prog, uint8_t *vuCode, uint32_t codeSize,
                                  uint8_t *vuData, uint32_t dataSize, uint32_t startPC,
                                  uint32_t maxCycles, uint32_t &ran)
{
    static uint64_t s_invocations = 0;
    static uint64_t s_mismatches = 0;
    static uint32_t s_reported = 0;

    const VU1State state0 = m_state;
    FlagSnapshot ring0[4];
    std::memcpy(ring0, m_flagRing, sizeof(ring0));
    const int ringPos0 = m_flagRingPos;
    const FlagSnapshot visible0 = m_flagVisible;
    const std::vector<uint8_t> data0(vuData, vuData + dataSize);

    // Pass 1: the compiled blocks (and the interpreter where they stop).
    std::vector<uint64_t> kicks1, kicks2;
    m_kickLog = &kicks1;
    uint32_t ran1 = 0;
    bool ended1 = aotRun(vuCode, codeSize, maxCycles, ran1);
    if (!ended1 && ran1 < maxCycles && m_state.pc + 8u <= codeSize)
    {
        m_aotLeanResume = true;
        run(vuCode, codeSize, vuData, dataSize, *m_curGs, m_curMemory, maxCycles - ran1);
        m_aotLeanResume = false;
    }
    else
    {
        qFlush();
    }
    const VU1State state1 = m_state;
    const std::vector<uint8_t> data1(vuData, vuData + dataSize);

    // Pass 2: the program, then whatever it hands over to.
    m_state = state0;
    std::memcpy(m_flagRing, ring0, sizeof(ring0));
    m_flagRingPos = ringPos0;
    m_flagVisible = visible0;
    std::memcpy(vuData, data0.data(), dataSize);
    m_kickLog = &kicks2;
    m_suppressSideEffects = true;
    uint32_t ended2 = 0;
    uint32_t ran2 = prog->fn(*this, vuData, dataSize - 1u, startPC - prog->entry, maxCycles, ended2);
    if (!ended2)
    {
        uint32_t more = 0;
        ended2 = aotRun(vuCode, codeSize, maxCycles - ran2, more) ? 1u : 0u;
        ran2 += more;
        if (!ended2 && ran2 < maxCycles && m_state.pc + 8u <= codeSize)
        {
            m_aotLeanResume = true;
            run(vuCode, codeSize, vuData, dataSize, *m_curGs, m_curMemory, maxCycles - ran2);
            m_aotLeanResume = false;
        }
    }
    m_suppressSideEffects = false;
    m_kickLog = nullptr;

    ++s_invocations;
    const bool sameState = std::memcmp(&m_state, &state1, sizeof(VU1State)) == 0;
    const bool sameData = std::memcmp(vuData, data1.data(), dataSize) == 0;
    const bool sameKicks = kicks1 == kicks2;
    if (!sameState || !sameData || !sameKicks)
    {
        ++s_mismatches;
        if (s_reported < 16u)
        {
            ++s_reported;
            std::fprintf(stderr,
                         "[vu-prog-verify] MISMATCH #%llu program %u entry=%04x (state=%s data=%s kicks=%s %zu/%zu)\n",
                         (unsigned long long)s_mismatches, prog->id, startPC, sameState ? "ok" : "DIFF",
                         sameData ? "ok" : "DIFF", sameKicks ? "ok" : "DIFF", kicks1.size(), kicks2.size());
            for (uint32_t r = 0; r < 32u; ++r)
                if (std::memcmp(m_state.vf[r], state1.vf[r], 16) != 0)
                {
                    const auto *g = reinterpret_cast<const uint32_t *>(m_state.vf[r]);
                    const auto *e = reinterpret_cast<const uint32_t *>(state1.vf[r]);
                    std::fprintf(stderr,
                                 "[vu-prog-verify]   vf%02u prog=%08x,%08x,%08x,%08x ref=%08x,%08x,%08x,%08x\n",
                                 r, g[0], g[1], g[2], g[3], e[0], e[1], e[2], e[3]);
                }
            for (uint32_t r = 0; r < 16u; ++r)
                if (m_state.vi[r] != state1.vi[r])
                    std::fprintf(stderr, "[vu-prog-verify]   vi%02u prog=%08x ref=%08x\n", r,
                                 (uint32_t)m_state.vi[r], (uint32_t)state1.vi[r]);
            if (std::memcmp(m_state.acc, state1.acc, 16) != 0)
            {
                const auto *g = reinterpret_cast<const uint32_t *>(m_state.acc);
                const auto *e = reinterpret_cast<const uint32_t *>(state1.acc);
                std::fprintf(stderr, "[vu-prog-verify]   acc prog=%08x,%08x,%08x,%08x ref=%08x,%08x,%08x,%08x\n",
                             g[0], g[1], g[2], g[3], e[0], e[1], e[2], e[3]);
            }
            uint32_t qa, qb, ia, ib;
            std::memcpy(&qa, &m_state.q, 4);
            std::memcpy(&qb, &state1.q, 4);
            std::memcpy(&ia, &m_state.i, 4);
            std::memcpy(&ib, &state1.i, 4);
            std::fprintf(stderr,
                         "[vu-prog-verify]   prog pc=%04x q=%08x i=%08x mac=%08x status=%08x clip=%06x ebit=%d bp=%d bt=%04x | "
                         "ref pc=%04x q=%08x i=%08x mac=%08x status=%08x clip=%06x ebit=%d bp=%d bt=%04x\n",
                         m_state.pc, qa, ia, m_state.mac, m_state.status, m_state.clip, (int)m_state.ebit,
                         (int)m_state.branchPending, m_state.branchTarget, state1.pc, qb, ib, state1.mac,
                         state1.status, state1.clip, (int)state1.ebit, (int)state1.branchPending,
                         state1.branchTarget);
            uint32_t shown = 0;
            for (uint32_t o = 0; o + 16u <= dataSize && shown < 8u; o += 16u)
                if (std::memcmp(vuData + o, data1.data() + o, 16) != 0)
                {
                    uint32_t g[4], e[4];
                    std::memcpy(g, vuData + o, 16);
                    std::memcpy(e, data1.data() + o, 16);
                    std::fprintf(stderr,
                                 "[vu-prog-verify]   mem[%04x] prog=%08x,%08x,%08x,%08x ref=%08x,%08x,%08x,%08x\n",
                                 o, g[0], g[1], g[2], g[3], e[0], e[1], e[2], e[3]);
                    ++shown;
                }
        }
    }
    if ((s_invocations & 0xFFFFFu) == 0u || (s_mismatches != 0u && (s_invocations & 0x3FFFu) == 0u))
        std::fprintf(stderr, "[vu-prog-verify] %llu invocations, %llu mismatches\n",
                     (unsigned long long)s_invocations, (unsigned long long)s_mismatches);

    // The game keeps pass 1.
    m_state = state1;
    std::memcpy(vuData, data1.data(), dataSize);
    ran = ran1;
    return true;
}
