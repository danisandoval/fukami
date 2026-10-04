// rr5_car_builder.h -- native block loop of RR5's vertex builder func_222EB8.
// Included once by patches.cpp, inside its native-hot-function prelude (after rrv_native::Clock).
//
// WHAT THE GAME DOES
//   Cars (and a few other models: callers func_227298, func_221550, func_23A290) are not drawn by
//   VU1. func_222EB8 transforms every vertex on the EE: it streams the model part through the
//   scratchpad in chunks (toSPR DMA), walks the blocks of a chunk, and for each vertex stores the
//   packed position / normal / colour / UV as integers at 0x70000000, loads them into VF28 / VF30 /
//   VF12 / VF27, calls a VU0 microprogram (VCALLMS 0x00 / 0x10 / 0x20 / 0x30 / 0x40: transform,
//   lighting, clip test, environment map) and stores the results (ST, RGBA, XYZ + ADC) in a GIF
//   packet in the scratchpad, which a fromSPR destination-chain DMA then copies into the display
//   list. One VCALLMS and 26 to 44 EE instructions per vertex.
//
// WHAT WAS SLOW (host `sample`, 8-car race, 2026-10-02)
//   func_222EB8 was 32.5% of the game thread: a third of it the per-instruction clock and
//   register traffic of those EE instructions, and most of the rest the call path around the
//   compiled microcode (the VU0 register file moved into the interpreter and back, 1 KiB per
//   vertex, and the round-toward-zero switch, both per vertex).
//
// WHAT THIS IS
//   blockLoop() runs the block loop of one chunk (guest pc 0x223128 up to, not including,
//   0x223D8C) natively: the same loads, the same stores to the same scratchpad bytes in the same
//   order, the same microprograms on the same VU0 state, the same final register values, and
//   the guest clock advanced by exactly the number of instructions the generated code would have
//   executed. It is entered from the native copy of func_222EB8 through a generator hook
//   (tools/ee-native/ee_native_gen.py --hook); everything else in the function (the prologue,
//   the DMA kicks, the epilogue) stays generated code.
//
//   It takes blocks only when
//     * they fit in the armed lazy window (no checkpoint inside them could do anything but move
//       the owner clock: the test rrv_native::Clock::ck makes per instruction, made once for all
//       of them), and
//     * the chunk has the shape this code was written for (known block types, the jump table
//       unmodified, every access inside the scratchpad, input and output not overlapping).
//   The window ends at the next modeled event, and there is one at every horizontal blank edge,
//   so a chunk often does not fit whole. Then the blocks that do fit are taken and the function
//   is left at the head of the next block (0x223128 again), exactly as the generated code would
//   be there: the generated code runs that block, takes the event, and the hook is asked again
//   for the rest. Otherwise it returns false having changed nothing.
//
//   RRV_RR5_NATIVE_BUILDER=0 turns it off. RRV_RR5_NATIVE_BUILDER_VERIFY=1 computes every eligible
//   chunk natively on the side, lets the generated code run it, and compares the whole register
//   file, the VU0 state, the scratchpad and the clock at 0x223D8C (blockLoopCheck, a generator
//   probe): docs/TESTING.md T-CAR-BUILDER.
//
// This is RR5-specific (SLUS-20002): the block formats, the jump table at 0x31E930 and the
// colour table at 0x1DE64A0 are this game's. The native copy it hooks is only used when the
// guest code matches the generation words (rrv_native::wordsMatch).

namespace rrv_car {

constexpr uint32_t kSprBase = PS2_SCRATCHPAD_BASE;
constexpr uint32_t kSprSize = PS2_SCRATCHPAD_SIZE;
constexpr uint32_t kJumpTable = 0x31E930u;  // 17 handler addresses, indexed by block type
constexpr uint32_t kColorTable = 0x1DE64A0u; // one RGBA quadword per block type (types 10..16)
constexpr uint32_t kMaxBlocks = 512u;

// Guest instructions between two points of the generated function, counted from its listing
// (every executed instruction, delay slots included, costs one cycle).
constexpr uint32_t kHead = 10u;     // 0x223128..0x22314C: block header, type range test
constexpr uint32_t kDispatch = 7u;  // 0x223150..0x223168: jump through the table
constexpr uint32_t kTail = 5u;      // 0x223D68..0x223D78: align the input pointer, loop
constexpr uint32_t kEnd = 4u;       // 0x223D7C..0x223D88: the END tag after the last block

struct Shape
{
    uint32_t handler;  // jump table entry
    uint8_t stride;    // input bytes per vertex
    uint8_t reach;     // input bytes one vertex load reads
    uint8_t entry;     // VCALLMS address
    uint8_t outQw;     // output quadwords per vertex, per stream
    uint8_t streams;   // 1, or 2 (base pass + environment-map pass)
    uint8_t header;    // instructions before the vertex loop
    uint8_t loop;      // instructions per vertex
    uint8_t exit;      // instructions after the loop
};
// Types 1..9, then the shared handler of types 10..16.
constexpr Shape kShapes[11] = {
    {0x22316Cu, 0x10, 0x10, 0x00, 1, 1, 10, 7, 2}, // type 0: quadwords copied through (no vertices)
    {0x2231BCu, 0x0A, 0x0A, 0x10, 2, 1, 25, 26, 2},
    {0x22328Cu, 0x10, 0x10, 0x00, 2, 1, 31, 33, 2},
    {0x223390u, 0x0E, 0x0E, 0x10, 3, 1, 32, 34, 2},
    {0x22349Cu, 0x10, 0x10, 0x00, 3, 1, 35, 38, 2},
    {0x2235C4u, 0x12, 0x11, 0x00, 3, 1, 36, 39, 2},
    {0x2236F4u, 0x10, 0x10, 0x20, 3, 2, 44, 43, 2},
    {0x223858u, 0x12, 0x11, 0x20, 3, 2, 45, 44, 2},
    {0x2239C4u, 0x10, 0x10, 0x30, 3, 2, 45, 43, 2},
    {0x223B2Cu, 0x10, 0x10, 0x40, 3, 2, 44, 43, 2},
    {0x223C90u, 0x0A, 0x0A, 0x10, 3, 1, 28, 26, 0},
};
inline const Shape &shapeOf(uint32_t type) { return kShapes[type < 10u ? type : 10u]; }

struct Stats
{
    uint64_t chunks = 0, partial = 0, blocks = 0, vertices = 0; // taken natively (chunk ends, block runs)
    uint64_t window = 0, shape = 0;                // nothing taken, and why
    uint64_t verified = 0, mismatches = 0;         // RRV_RR5_NATIVE_BUILDER_VERIFY
};
inline Stats g_stats;

inline bool envOn(const char *name, bool fallback)
{
    const char *v = std::getenv(name);
    if (!v || !v[0])
        return fallback;
    return v[0] != '0';
}
inline const bool kEnabled = envOn("RRV_RR5_NATIVE_BUILDER", true);
inline const bool kVerify = envOn("RRV_RR5_NATIVE_BUILDER_VERIFY", false);

// A guest register as the generated code holds it: 128 bits, of which a 32- or 64-bit result
// replaces the low half only (SET_GPR_S32 / SET_GPR_U64), and LQ / QMFC2 replace all of it.
struct Reg
{
    uint64_t lo, hi;
};
inline Reg getReg(const R5900Context *ctx, int index)
{
    Reg r;
    std::memcpy(&r, &ctx->r[index], sizeof(r));
    return r;
}
inline void putReg(R5900Context *ctx, int index, const Reg &r) { std::memcpy(&ctx->r[index], &r, sizeof(r)); }
inline void putLow(R5900Context *ctx, int index, uint64_t lo) { std::memcpy(&ctx->r[index], &lo, sizeof(lo)); }
inline uint64_t sx32(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }

template <typename T>
inline T rd(const uint8_t *p)
{
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}
template <typename T>
inline void wr(uint8_t *p, T v) { std::memcpy(p, &v, sizeof(T)); }
inline uint64_t lh(const uint8_t *p) { return static_cast<uint64_t>(static_cast<int64_t>(rd<int16_t>(p))); }

// A run of blocks, measured without changing anything.
struct Plan
{
    uint64_t cycles = 0;  // guest instructions, the END tag included when the run ends the chunk
    uint32_t blocks = 0, vertices = 0;
    bool complete = false; // the run ends the chunk: continue at 0x223D8C
};

// The guest instructions one block costs, from its header (0 = not a block this code runs).
inline uint64_t blockCycles(uint32_t type, uint32_t n)
{
    uint64_t cycles = kHead + kTail;
    if (type >= 0x11u)
        return cycles;
    const Shape &s = shapeOf(type);
    cycles += kDispatch + s.header;
    if (type == 0u)
        return n != 0u ? cycles + 1u + uint64_t(s.loop) * n + s.exit : cycles;
    // The vertex loop is a do-while: a count of zero would run 2^32 times.
    return n != 0u ? cycles + uint64_t(s.loop) * n + s.exit : 0u;
}
inline uint32_t blockInput(uint32_t type, uint32_t n)
{
    return 0x10u + (type >= 0x11u ? 0u : type == 0u ? 0x10u * n : uint32_t(shapeOf(type).stride) * n);
}

// Walks the block headers of the chunk at `in` (scratchpad offsets) and decides whether the native
// loop reproduces it: `whole` is the chunk. `rdram` is read for the jump table only.
inline bool shape(const uint8_t *rdram, const uint8_t *spr, uint32_t in, uint32_t out, Plan &whole)
{
    const uint32_t in0 = in, out0 = out;
    if ((in & 15u) != 0u || (out & 15u) != 0u || in < 0x40u || out < 0x40u)
        return false;
    for (;;)
    {
        if (whole.blocks >= kMaxBlocks || in + 0x10u > kSprSize)
            return false;
        const uint8_t *h = spr + in;
        const uint32_t type = h[15], n = rd<uint16_t>(h) & 0x7FFFu;
        const uint64_t cycles = blockCycles(type, n);
        if (cycles == 0u)
            return false;
        whole.cycles += cycles;
        in += blockInput(type, n);
        if (type < 0x11u)
        {
            const Shape &s = shapeOf(type);
            if (rd<uint32_t>(rdram + kJumpTable + type * 4u) != s.handler)
                return false;
            if (type == 0u)
            {
                out += 0x20u + 0x10u * n;
                if (in > kSprSize)
                    return false;
            }
            else
            {
                whole.vertices += n;
                // Each pass also loads the vertex after its last one (the game's loop is unrolled
                // that way): `reach` bytes at the block's end.
                if (in + s.reach > kSprSize)
                    return false;
                out += (0x20u + 0x10u * s.outQw * n) * s.streams;
            }
        }
        if (out + 0x10u > kSprSize)
            return false;
        in = (in + 15u) & ~15u;
        ++whole.blocks;
        if ((h[1] & 0x80u) != 0u)
            break;
    }
    whole.cycles += kEnd;
    whole.complete = true;
    // The chunk's own bytes and the output must not overlap: the headers were read here, ahead of
    // the stores. (The game lays the output right behind a 0x470-byte input buffer, so the load
    // of "the vertex after the last one" of a full chunk reads the first output quadwords. That
    // is reproduced: run() loads and stores in the game's order.)
    const uint32_t outEnd = out + 0x10u;
    return in <= out0 || outEnd <= in0;
}

// The leading blocks of a chunk `shape` accepted that cost at most `budget` instructions. Never
// the last block: the END tag after it is part of the same stretch, and the caller has already
// found that the whole chunk does not fit.
inline void prefix(const uint8_t *spr, uint32_t in, uint64_t budget, Plan &part)
{
    for (;;)
    {
        const uint8_t *h = spr + in;
        const uint32_t type = h[15], n = rd<uint16_t>(h) & 0x7FFFu;
        const uint64_t cycles = blockCycles(type, n);
        if ((h[1] & 0x80u) != 0u || part.cycles + cycles > budget)
            return;
        part.cycles += cycles;
        part.vertices += (type != 0u && type < 0x11u) ? n : 0u;
        ++part.blocks;
        in = (in + blockInput(type, n) + 15u) & ~15u;
    }
}

// Guest registers the block loop reads or writes, held in host variables for the chunk.
struct Regs
{
    Reg v0, a3, t0, t3;                     // may take a 128-bit value (LQ, QMFC2)
    uint64_t v1, a0, a1, t1, t2, t4, s2;    // low halves
    uint32_t t5, a2, s3, t6;                // pointers: scratchpad input / output, display list
    uint64_t lo, hi;
    bool loHi = false;                      // a type 6..9 header ran its MULT
};

// The integer vertex fields the game stages at 0x70000000 before each microprogram, per block type
// family. Every function is the game's own load/store sequence for one vertex at `p`: the values
// left in t0..t3 are the ones the generated code leaves there.
#define RRV_CAR_XYZ()                                                                           \
    do                                                                                          \
    {                                                                                           \
        r.t0.lo = lh(p);                                                                        \
        r.t1 = lh(p + 2);                                                                       \
        r.t2 = lh(p + 4);                                                                       \
        wr<uint32_t>(spr + 0x00, static_cast<uint32_t>(r.t0.lo));                               \
        wr<uint32_t>(spr + 0x04, static_cast<uint32_t>(r.t1));                                  \
        wr<uint32_t>(spr + 0x08, static_cast<uint32_t>(r.t2));                                  \
        wr<uint32_t>(spr + 0x0C, static_cast<uint32_t>(r.t3.lo));                               \
    } while (0)
#define RRV_CAR_NORMAL()                                                                        \
    do                                                                                          \
    {                                                                                           \
        r.t0.lo = lh(p + 6);                                                                    \
        r.t1 = lh(p + 8);                                                                       \
        r.t2 = lh(p + 10);                                                                      \
        wr<uint32_t>(spr + 0x10, static_cast<uint32_t>(r.t0.lo));                               \
        wr<uint32_t>(spr + 0x14, static_cast<uint32_t>(r.t1));                                  \
        wr<uint32_t>(spr + 0x18, static_cast<uint32_t>(r.t2));                                  \
    } while (0)
#define RRV_CAR_RGBA(off)                                                                       \
    do                                                                                          \
    {                                                                                           \
        r.t0.lo = p[(off)];                                                                     \
        r.t1 = p[(off) + 1];                                                                    \
        r.t2 = p[(off) + 2];                                                                    \
        r.t3.lo = p[(off) + 3];                                                                 \
        wr<uint32_t>(spr + 0x20, static_cast<uint32_t>(r.t0.lo));                               \
        wr<uint32_t>(spr + 0x24, static_cast<uint32_t>(r.t1));                                  \
        wr<uint32_t>(spr + 0x28, static_cast<uint32_t>(r.t2));                                  \
        wr<uint32_t>(spr + 0x2C, static_cast<uint32_t>(r.t3.lo));                               \
    } while (0)
#define RRV_CAR_UV(off)                                                                         \
    do                                                                                          \
    {                                                                                           \
        r.t0.lo = lh(p + (off));                                                                \
        r.t1 = lh(p + (off) + 2);                                                               \
        r.t2 = 0x1000u;                                                                         \
        wr<uint32_t>(spr + 0x30, static_cast<uint32_t>(r.t0.lo));                               \
        wr<uint32_t>(spr + 0x34, static_cast<uint32_t>(r.t1));                                  \
        wr<uint32_t>(spr + 0x38, 0x1000u);                                                      \
    } while (0)
#define RRV_CAR_GREY()                                                                          \
    do                                                                                          \
    {                                                                                           \
        r.t0.lo = 0x80u;                                                                        \
        wr<uint32_t>(spr + 0x20, 0x80u);                                                        \
        wr<uint32_t>(spr + 0x24, 0x80u);                                                        \
        wr<uint32_t>(spr + 0x28, 0x80u);                                                        \
        wr<uint32_t>(spr + 0x2C, 0x80u);                                                        \
    } while (0)
#define RRV_CAR_GREY_ALPHA(off)                                                                 \
    do                                                                                          \
    {                                                                                           \
        r.t0.lo = 0x80u;                                                                        \
        r.t1 = p[(off)];                                                                        \
        wr<uint32_t>(spr + 0x20, 0x80u);                                                        \
        wr<uint32_t>(spr + 0x24, 0x80u);                                                        \
        wr<uint32_t>(spr + 0x28, 0x80u);                                                        \
        wr<uint32_t>(spr + 0x2C, static_cast<uint32_t>(r.t1));                                  \
    } while (0)

__attribute__((always_inline)) inline void loadVertex(uint32_t type, const uint8_t *p, uint8_t *spr, Regs &r)
{
    switch (type)
    {
    case 1: // position, colour
        RRV_CAR_XYZ();
        RRV_CAR_RGBA(6);
        break;
    case 2: // position, normal, colour
        RRV_CAR_XYZ();
        RRV_CAR_NORMAL();
        RRV_CAR_RGBA(12);
        break;
    case 3: // position, colour, UV
        RRV_CAR_XYZ();
        RRV_CAR_RGBA(6);
        RRV_CAR_UV(10);
        break;
    case 4: // position, normal, UV; colour 0x80
    case 6:
    case 8:
    case 9:
        RRV_CAR_XYZ();
        RRV_CAR_NORMAL();
        if (type == 4u)
        {
            RRV_CAR_GREY();
            RRV_CAR_UV(12);
        }
        else
        {
            RRV_CAR_UV(12);
            RRV_CAR_GREY();
        }
        break;
    case 5: // position, normal, UV, alpha
    case 7:
        RRV_CAR_XYZ();
        RRV_CAR_NORMAL();
        RRV_CAR_UV(12);
        RRV_CAR_GREY_ALPHA(16);
        break;
    default: // 10..16: position, UV; the colour comes from the table
        RRV_CAR_XYZ();
        RRV_CAR_UV(6);
        break;
    }
}
#undef RRV_CAR_XYZ
#undef RRV_CAR_NORMAL
#undef RRV_CAR_RGBA
#undef RRV_CAR_UV
#undef RRV_CAR_GREY
#undef RRV_CAR_GREY_ALPHA

// The block loop: `plan.blocks` blocks from guest pc 0x223128, and the END tag (through 0x223D88)
// when they end the chunk. `shape` accepted the chunk.
inline void run(uint8_t *rdram, uint8_t *spr, R5900Context *ctx, PS2Runtime *runtime, VU1State &vu, Regs &r,
                const Plan &plan)
{
    static_assert(sizeof(vu.vf[0]) == 16, "VF register layout");
    for (uint32_t block = 0; block < plan.blocks; ++block)
    {
        // 0x223128: the block header (a GIF tag: vertex count, last-block bit, type in byte 15).
        const uint8_t *h = spr + (r.t5 - kSprBase);
        const uint32_t type = h[15];
        r.t0.lo = type;
        r.s2 = h[1] & 0x80u;
        std::memcpy(&r.a3, h, 16);
        uint32_t n = rd<uint16_t>(h) & 0x7FFFu;
        r.t4 = n;
        r.t5 += 0x10u;
        if (type < 0x11u)
        {
            const Shape &s = shapeOf(type);
            uint8_t *out = spr + (r.a2 - kSprBase);
            r.a0 = sx32(s.handler);
            if (type == 0u)
            {
                wr<uint32_t>(out + 4, r.s3);
                std::memcpy(out + 0x10, &r.a3, 16);
                r.s3 += 0x10u;
                wr<uint32_t>(out, (n + 1u) | 0x10000000u);
                wr<uint64_t>(out + 8, 0u);
                r.a2 += 0x20u;
                r.v0.lo = 0x10000000u;
                for (; n != 0u; --n)
                {
                    std::memcpy(&r.v0, spr + (r.t5 - kSprBase), 16);
                    r.s3 += 0x10u;
                    r.t5 += 0x10u;
                    std::memcpy(spr + (r.a2 - kSprBase), &r.v0, 16);
                    r.a2 += 0x10u;
                }
                r.t4 = 0;
            }
            else
            {
                // The destination-chain tag (CNT, count, display-list address) and the block's GIF tag.
                const uint32_t tag = (uint32_t(s.outQw) * n + 1u) | 0x10000000u;
                const uint32_t step = 0x10u * s.outQw;
                uint8_t *out2 = nullptr;
                if (s.streams == 2u)
                {
                    const int64_t product = static_cast<int64_t>(static_cast<int32_t>(n)) * 0x30;
                    r.lo = sx32(static_cast<uint32_t>(product));
                    r.hi = sx32(static_cast<uint32_t>(product >> 32));
                    r.loHi = true;
                    r.a0 = sx32(static_cast<uint32_t>(product));
                    wr<uint32_t>(out + 4, r.s3);
                    wr<uint32_t>(out, tag);
                    r.s3 += 0x10u;
                    wr<uint64_t>(out + 8, 0u);
                    std::memcpy(out + 0x10, &r.a3, 16);
                    r.a2 += 0x20u;
                    out += 0x20;
                    out2 = out + static_cast<uint32_t>(product);
                    wr<uint32_t>(out2 + 4, r.t6);
                    wr<uint32_t>(out2, tag);
                    r.t6 += 0x10u;
                    std::memcpy(out2 + 0x10, &r.a3, 16);
                    wr<uint64_t>(out2 + 8, 0u);
                    out2 += 0x20;
                    if (type == 8u)
                    {
                        // 0x223A14: VMULx.w VF5, VF0, VF5x, a macro-mode op (EE rounding mode).
                        __m128 vf0, vf5;
                        std::memcpy(&vf0, vu.vf[0], 16);
                        std::memcpy(&vf5, vu.vf[5], 16);
                        const __m128 res = PS2_VMUL(vf0, _mm_shuffle_ps(vf5, vf5, _MM_SHUFFLE(0, 0, 0, 0)));
                        const __m128i mask = _mm_set_epi32(-1, 0, 0, 0);
                        vf5 = _mm_blendv_ps(vf5, res, _mm_castsi128_ps(mask));
                        std::memcpy(vu.vf[5], &vf5, 16);
                    }
                }
                else
                {
                    if (type >= 10u)
                    {
                        r.a1 = 0x10000000u;
                        r.a0 = sx32((type << 4) + kColorTable);
                    }
                    wr<uint32_t>(out + 4, r.s3);
                    std::memcpy(out + 0x10, &r.a3, 16);
                    wr<uint32_t>(out, tag);
                    wr<uint64_t>(out + 8, 0u);
                    r.s3 += 0x10u;
                    r.a2 += 0x20u;
                    out += 0x20;
                }
                const uint8_t *p = spr + (r.t5 - kSprBase);
                loadVertex(type, p, spr, r);
                const uint8_t *color = rdram + (kColorTable + (type << 4));
                // The microprograms run round-toward-zero; one scope for the block instead of one
                // per vertex (the scope each call opens is then a no-op).
                const rrv::fp::ScopedRoundTowardZero rounding(true);
                do
                {
                    std::memcpy(vu.vf[28], spr + 0x00, 16);
                    switch (type)
                    {
                    case 1:
                        std::memcpy(vu.vf[12], spr + 0x20, 16);
                        break;
                    case 2:
                        std::memcpy(vu.vf[30], spr + 0x10, 16);
                        std::memcpy(vu.vf[12], spr + 0x20, 16);
                        break;
                    case 3:
                        std::memcpy(vu.vf[12], spr + 0x20, 16);
                        std::memcpy(vu.vf[27], spr + 0x30, 16);
                        break;
                    case 4:
                    case 5:
                    case 6:
                    case 7:
                    case 8:
                    case 9:
                        std::memcpy(vu.vf[30], spr + 0x10, 16);
                        std::memcpy(vu.vf[12], spr + 0x20, 16);
                        std::memcpy(vu.vf[27], spr + 0x30, 16);
                        break;
                    default:
                        std::memcpy(vu.vf[27], spr + 0x30, 16);
                        break;
                    }
                    runtime->vu0BurstRunV1(ctx, s.entry);
                    r.t5 += s.stride;
                    p += s.stride;
                    // The game loads the next vertex before it stores this one.
                    if (type >= 10u)
                    {
                        r.t0.lo = lh(p);
                        r.t1 = lh(p + 2);
                        r.t2 = lh(p + 4);
                        wr<uint32_t>(spr + 0x00, static_cast<uint32_t>(r.t0.lo));
                        wr<uint32_t>(spr + 0x04, static_cast<uint32_t>(r.t1));
                        wr<uint32_t>(spr + 0x08, static_cast<uint32_t>(r.t2));
                        wr<uint32_t>(spr + 0x0C, static_cast<uint32_t>(r.t3.lo));
                        r.t0.lo = lh(p + 6);
                        r.t1 = lh(p + 8);
                        std::memcpy(&r.t3, color, 16);
                        r.t2 = 0x1000u;
                        wr<uint32_t>(spr + 0x30, static_cast<uint32_t>(r.t0.lo));
                        wr<uint32_t>(spr + 0x34, static_cast<uint32_t>(r.t1));
                        wr<uint32_t>(spr + 0x38, 0x1000u);
                        std::memcpy(out + 0x10, &r.t3, 16);
                    }
                    else
                    {
                        loadVertex(type, p, spr, r);
                    }
                    --n;
                    out += step;
                    r.a2 += step;
                    r.s3 += step;
                    std::memcpy(&r.t0, vu.vf[31], 16); // QMFC2 t0, VF31
                    if (s.outQw == 2u)
                    {
                        std::memcpy(out - 0x20, vu.vf[12], 16);
                        std::memcpy(out - 0x10, vu.vf[31], 16);
                    }
                    else if (type >= 10u)
                    {
                        std::memcpy(out - 0x30, vu.vf[27], 16);
                        std::memcpy(out - 0x10, vu.vf[31], 16);
                    }
                    else
                    {
                        std::memcpy(out - 0x30, vu.vf[27], 16);
                        std::memcpy(out - 0x20, vu.vf[12], 16);
                        std::memcpy(out - 0x10, vu.vf[31], 16);
                        if (out2)
                        {
                            out2 += step;
                            r.t6 += step;
                            std::memcpy(out2 - 0x30, vu.vf[30], 16);
                            std::memcpy(out2 - 0x20, vu.vf[12], 16);
                            std::memcpy(out2 - 0x10, vu.vf[31], 16);
                        }
                    }
                } while (n != 0u);
                r.t4 = 0;
                if (out2)
                    r.a2 = kSprBase + static_cast<uint32_t>(out2 - spr); // 0x223854: a2 = v0
            }
        }
        // 0x223D68: round the input pointer up to the next quadword.
        r.v0.lo = 0xFFFFFFFFFFFFFFF0ull;
        r.v1 = sx32(r.t5 + 0xFu);
        r.t5 = static_cast<uint32_t>(r.v1) & 0xFFFFFFF0u;
    }
    if (!plan.complete)
        return; // at 0x223128, the head of the next block
    // 0x223D7C: the END tag of the destination chain.
    uint8_t *end = spr + (r.a2 - kSprBase);
    r.v0.lo = 0x70000000u;
    wr<uint64_t>(end + 8, 0u);
    wr<uint32_t>(end, 0x70000000u);
    wr<uint32_t>(end + 4, 0u);
}

inline void loadRegs(const R5900Context *ctx, Regs &r)
{
    r.v0 = getReg(ctx, 2);
    r.a3 = getReg(ctx, 7);
    r.t0 = getReg(ctx, 8);
    r.t3 = getReg(ctx, 11);
    r.v1 = getReg(ctx, 3).lo;
    r.a0 = getReg(ctx, 4).lo;
    r.a1 = getReg(ctx, 5).lo;
    r.t1 = getReg(ctx, 9).lo;
    r.t2 = getReg(ctx, 10).lo;
    r.t4 = getReg(ctx, 12).lo;
    r.s2 = getReg(ctx, 18).lo;
    r.t5 = static_cast<uint32_t>(getReg(ctx, 13).lo);
    r.a2 = static_cast<uint32_t>(getReg(ctx, 6).lo);
    r.s3 = static_cast<uint32_t>(getReg(ctx, 19).lo);
    r.t6 = static_cast<uint32_t>(getReg(ctx, 14).lo);
    r.lo = ctx->lo;
    r.hi = ctx->hi;
    r.loHi = false;
}

inline void storeRegs(R5900Context *ctx, const Regs &r)
{
    putReg(ctx, 2, r.v0);
    putReg(ctx, 7, r.a3);
    putReg(ctx, 8, r.t0);
    putReg(ctx, 11, r.t3);
    putLow(ctx, 3, r.v1);
    putLow(ctx, 4, r.a0);
    putLow(ctx, 5, r.a1);
    putLow(ctx, 9, r.t1);
    putLow(ctx, 10, r.t2);
    putLow(ctx, 12, r.t4);
    putLow(ctx, 18, r.s2);
    putLow(ctx, 13, sx32(r.t5));
    putLow(ctx, 6, sx32(r.a2));
    putLow(ctx, 19, sx32(r.s3));
    putLow(ctx, 14, sx32(r.t6));
    if (r.loHi)
    {
        ctx->lo = r.lo;
        ctx->hi = r.hi;
    }
    // The last branch the loop executes is the BEQZ at 0x223D74 (taken to the next block, or not
    // taken after the last one).
    ctx->in_delay_slot = false;
    ctx->branch_pc = 0x223D74u;
}

// RRV_RR5_NATIVE_BUILDER_VERIFY: what the native loop computed for the blocks the generated code
// is now running.
struct Expect
{
    bool pending = false;
    bool vuRan = false;
    uint32_t blocksLeft = 0; // 0: compared at 0x223D8C; else at the hook, that many blocks later
    uint64_t cycles = 0;
    alignas(16) uint8_t ctx[sizeof(R5900Context)];
    uint8_t vu[sizeof(VU1State)];
    uint8_t spr[kSprSize];
};
inline Expect g_expect;

inline void execute(uint8_t *rdram, uint8_t *spr, R5900Context *ctx, PS2Runtime *runtime, const Plan &p)
{
    Regs r;
    loadRegs(ctx, r);
    VU1State &vu = runtime->vu0BurstBeginV1(ctx);
    run(rdram, spr, ctx, runtime, vu, r, p);
    runtime->vu0BurstEndV1(ctx);
    storeRegs(ctx, r);
}

__attribute__((noinline)) inline void compare(R5900Context *ctx, PS2Runtime *runtime, const uint8_t *spr, uint64_t cycle)
{
    g_expect.pending = false;
    ++g_stats.verified;
    alignas(16) uint8_t actual[sizeof(R5900Context)];
    std::memcpy(actual, ctx, sizeof(R5900Context));
    // Not part of the comparison: the pc (the generated code sets it per instruction) and the
    // context's instruction count (published at the next commit; the clock is compared instead).
    std::memcpy(actual + offsetof(R5900Context, pc), g_expect.ctx + offsetof(R5900Context, pc), sizeof(uint32_t));
    std::memcpy(actual + offsetof(R5900Context, insn_count), g_expect.ctx + offsetof(R5900Context, insn_count),
                sizeof(uint64_t));
    const bool ctxOk = std::memcmp(actual, g_expect.ctx, sizeof(R5900Context)) == 0;
    const bool vuOk = !g_expect.vuRan || std::memcmp(&runtime->vu0StateV1(), g_expect.vu, sizeof(VU1State)) == 0;
    const bool sprOk = std::memcmp(spr, g_expect.spr, kSprSize) == 0;
    const bool clockOk = cycle == g_expect.cycles;
    if (ctxOk && vuOk && sprOk && clockOk)
        return;
    if (++g_stats.mismatches > 20u)
        return;
    std::fprintf(stderr, "[rr5-car] VERIFY MISMATCH #%llu ctx=%d vu0=%d scratchpad=%d clock=%d (native %llu, generated %llu)\n",
                 static_cast<unsigned long long>(g_stats.mismatches), ctxOk, vuOk, sprOk, clockOk,
                 static_cast<unsigned long long>(g_expect.cycles), static_cast<unsigned long long>(cycle));
    if (!ctxOk)
        for (size_t i = 0; i < sizeof(R5900Context); ++i)
            if (actual[i] != g_expect.ctx[i])
            {
                std::fprintf(stderr, "[rr5-car]   context byte 0x%zx (gpr %zu if < 0x200): generated %02x native %02x\n", i,
                             i / 16u, actual[i], g_expect.ctx[i]);
                break;
            }
    if (!vuOk)
    {
        const uint8_t *live = reinterpret_cast<const uint8_t *>(&runtime->vu0StateV1());
        for (size_t i = 0; i < sizeof(VU1State); ++i)
            if (live[i] != g_expect.vu[i])
            {
                std::fprintf(stderr, "[rr5-car]   VU0 state byte 0x%zx: generated %02x native %02x\n", i, live[i],
                             g_expect.vu[i]);
                break;
            }
    }
    if (!sprOk)
        for (size_t i = 0; i < kSprSize; ++i)
            if (spr[i] != g_expect.spr[i])
            {
                std::fprintf(stderr, "[rr5-car]   scratchpad 0x%zx: generated %02x native %02x\n", i, spr[i],
                             g_expect.spr[i]);
                break;
            }
}

// What the block loop took: `cycles` guest instructions (0 = nothing changed); `complete` = the
// chunk is done, continue at 0x223D8C.
struct Taken
{
    uint64_t cycles;
    bool complete;
};

// The block loop for the chunk at 0x223128, with `budget` instructions left in the lazy window at
// guest cycle `cycle`. Out of line and handed scalars only: the caller's clock must not escape
// (rrv_native::Clock).
__attribute__((noinline)) inline Taken blockLoopRun(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint8_t *spr,
                                                    uint64_t cycle, uint64_t budget)
{
    if (__builtin_expect(g_expect.pending && g_expect.blocksLeft != 0u, false) && --g_expect.blocksLeft == 0u)
        compare(ctx, runtime, spr, cycle);
    const uint32_t in = static_cast<uint32_t>(getReg(ctx, 13).lo) - kSprBase;
    const uint32_t out = static_cast<uint32_t>(getReg(ctx, 6).lo) - kSprBase;
    Plan p;
    if (!spr || !rrv_native::kSprFast || g_ps2PathWatchArmed || !runtime->vu0BurstAvailableV1() ||
        static_cast<uint32_t>(getReg(ctx, 16).lo) != kSprBase || in >= kSprSize || out >= kSprSize ||
        static_cast<uint32_t>(getReg(ctx, 19).lo) >= 0x40000000u ||
        static_cast<uint32_t>(getReg(ctx, 14).lo) >= 0x40000000u || !shape(rdram, spr, in, out, p))
    {
        ++g_stats.shape;
        return Taken{0u, false};
    }
    if (p.cycles > budget)
    {
        p = Plan();
        prefix(spr, in, budget, p);
        if (p.blocks == 0u)
        {
            ++g_stats.window;
            return Taken{0u, false};
        }
    }
    if (__builtin_expect(kVerify, false))
    {
        if (g_expect.pending)
            return Taken{0u, false}; // a comparison is still out: let the generated code reach it
        static R5900Context savedCtx;
        static uint8_t savedSpr[kSprSize];
        static VU1State savedVu;
        std::memcpy(static_cast<void *>(&savedCtx), ctx, sizeof(savedCtx));
        std::memcpy(savedSpr, spr, kSprSize);
        std::memcpy(static_cast<void *>(&savedVu), &runtime->vu0StateV1(), sizeof(savedVu));
        execute(rdram, spr, ctx, runtime, p);
        std::memcpy(g_expect.ctx, ctx, sizeof(R5900Context));
        std::memcpy(g_expect.vu, &runtime->vu0StateV1(), sizeof(VU1State));
        std::memcpy(g_expect.spr, spr, kSprSize);
        g_expect.cycles = cycle + p.cycles;
        g_expect.vuRan = p.vertices != 0u;
        g_expect.blocksLeft = p.complete ? 0u : p.blocks;
        g_expect.pending = true;
        std::memcpy(static_cast<void *>(ctx), &savedCtx, sizeof(savedCtx));
        std::memcpy(spr, savedSpr, kSprSize);
        std::memcpy(static_cast<void *>(&runtime->vu0StateV1()), &savedVu, sizeof(savedVu));
        return Taken{0u, false};
    }
    execute(rdram, spr, ctx, runtime, p);
    ++(p.complete ? g_stats.chunks : g_stats.partial);
    g_stats.blocks += p.blocks;
    g_stats.vertices += p.vertices;
    return Taken{p.cycles, p.complete};
}

// The hook at 0x223128 of the native func_222EB8. True: the chunk is done and the clock advanced;
// the caller continues at 0x223D8C. False: the caller runs the block at 0x223128; either nothing
// changed, or the blocks before it were taken (context, memory and clock are then those of the
// generated code arriving at 0x223128 for this block).
__attribute__((always_inline)) inline bool blockLoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                                     rrv_native::Clock &clock)
{
    if (!kEnabled)
        return false;
    // rrv_native::Clock::ck for every instruction of the run at once: the last one checks at
    // c + cycles - 1, which must be below the window's end.
    const uint64_t budget = (clock.lim > clock.c && clock.quiet()) ? clock.lim - clock.c : 0u;
    const Taken taken = blockLoopRun(rdram, ctx, runtime, clock.spr_, clock.c, budget);
    if (taken.cycles == 0u)
        return false;
    clock.c += taken.cycles;
    // The last checkpoint: before the SW at 0x223D88 (an ordinary instruction), or before the
    // BEQZ at 0x223D74, whose delay slot counts a cycle without one.
    clock.lastck = clock.c - (taken.complete ? 1u : 2u);
    clock.any = true;
    return taken.complete;
}

__attribute__((noinline)) inline void blockLoopCheckPending(R5900Context *ctx, PS2Runtime *runtime, const uint8_t *spr,
                                                             uint64_t cycle)
{
    if (g_expect.blocksLeft != 0u)
    {
        // A block run that was to be compared at a later block head never got there.
        g_expect.pending = false;
        ++g_stats.verified;
        if (++g_stats.mismatches <= 20u)
            std::fprintf(stderr, "[rr5-car] VERIFY MISMATCH #%llu: chunk ended %u blocks before the native run's end\n",
                         static_cast<unsigned long long>(g_stats.mismatches), g_expect.blocksLeft);
        return;
    }
    compare(ctx, runtime, spr, cycle);
}

// The probe at 0x223D8C (verify mode): the generated code has just finished the chunk.
__attribute__((always_inline)) inline void blockLoopCheck(uint8_t *, R5900Context *ctx, PS2Runtime *runtime,
                                                          rrv_native::Clock &clock)
{
    if (__builtin_expect(g_expect.pending, false))
        blockLoopCheckPending(ctx, runtime, clock.spr_, clock.c);
}

inline void reportAtExit()
{
    std::fprintf(stderr,
                 "[rr5-car] builder: native chunk ends=%llu block runs=%llu blocks=%llu vertices=%llu, nothing taken: window=%llu shape=%llu",
                 static_cast<unsigned long long>(g_stats.chunks), static_cast<unsigned long long>(g_stats.partial),
                 static_cast<unsigned long long>(g_stats.blocks),
                 static_cast<unsigned long long>(g_stats.vertices), static_cast<unsigned long long>(g_stats.window),
                 static_cast<unsigned long long>(g_stats.shape));
    if (kVerify)
        std::fprintf(stderr, " verify: comparisons=%llu mismatches=%llu", static_cast<unsigned long long>(g_stats.verified),
                     static_cast<unsigned long long>(g_stats.mismatches));
    std::fputc('\n', stderr);
    std::fprintf(stderr,
                 "[gate4-lazy] plain bus accesses (VU0 memory, scratchpad DMA): %llu inside the window, %llu committed; "
                 "HLE charges: %llu inside the window, %llu committed\n",
                 static_cast<unsigned long long>(PS2Runtime::gate3LazyWorkCountV1(0)),
                 static_cast<unsigned long long>(PS2Runtime::gate3LazyWorkCountV1(1)),
                 static_cast<unsigned long long>(PS2Runtime::gate3LazyWorkCountV1(2)),
                 static_cast<unsigned long long>(PS2Runtime::gate3LazyWorkCountV1(3)));
}

} // namespace rrv_car
