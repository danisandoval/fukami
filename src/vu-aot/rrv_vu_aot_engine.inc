// rrv_vu_aot_engine.inc — statically recompiled VU microcode (RRV_VU_AOT).
//
// Included at the END of the staged ps2_vu1.cpp by scripts/vu_aot_overlay.py,
// after execUpperFastT / execLowerT are defined, so the generated blocks
// (generated/rr5/vu/rrv_vu_aot_blocks.inc, tools/vu-aot/vu_aot_gen.py) see
// the interpreter's own op handlers and every static helper they use.
//
// A generated block is a list of Aot::slot<LOWER, UPPER, N, FLAGS>(pc0)
// calls, keyed by its instruction words alone (position independent). Aot::slot below is VU1Interpreter::run()'s per-slot sequence, in the
// same order, with the instruction words as template constants:
//   capture + clear a pending branch (delay slot)   -> kMaybeBranch
//   flag-visibility ring: this slot reads ring[pos+N]
//   LOI -> I register
//   upper pipe: execUpperFastT<uop>
//   ring[pos+N] = this slot's {mac, clip, status}
//   lower pipe: execLowerT<op, funct, funct2>, or a NOP clearing m_updateMac
//   VF0 = (0,0,0,1), VI0 = 0
//   last slot: pc = taken branch target, or the fall-through pc
//   E bit -> state.ebit (the delay slot after it is the block's stop slot)
// The ring position is committed once per block (Aot::end), as run() would
// have left it after N slots. Nothing here decides semantics: every op is the
// interpreter's source, specialised by the compiler instead of a JIT.
//
// What a slot may leave out. The Steam Deck profile (docs/evidence/
// GATE5_STEAMDECK_PROFILE_2026-10-02.md) found 1.5% of block time in float
// arithmetic and the rest in flag words, operand clamps and bookkeeping that
// every slot paid whether or not anything read the result. The generator now
// analyses each block (tools/vu-aot/vu_aot_gen.py, "Block analysis") and passes
// what it proved in FLAGS:
//   kNoVisible   the lower op reads no flag, so the 4-slots-ago snapshot is not
//                fetched into m_flagVisible (scratch: run() and every reader
//                slot here write it before reading it, nothing else reads it);
//   kNoRingWrite no slot of this block reads this slot's snapshot and a later
//                slot of the block overwrites it;
//   kNoMacWord   the MAC word of this FMAC op reaches no snapshot that is kept
//                and is overwritten by a later FMAC op of the block;
//   kCleanVs/Vt/Acc  the operand lanes this op uses were written by an FMAC op
//                (or ITOF, or VF0) earlier in the block, so the operand clamp
//                is the identity.
// Two more are decided here, from the slot's own words: a slot whose lower op
// is a NOP (or an immediate) and whose upper op cannot write VF0 does not
// re-assert VF0 = (0,0,0,1) / VI0 = 0 (slot 0 always does: it establishes the
// invariant the block relies on), and does not publish its pc, which only the
// lower pipe and the CLIP diagnostic read.
// After the block's last slot the state is the interpreter's: VU1State, the
// four ring entries and the ring position. A block
// generated without the analysis (all option bits 0) runs every step, as before.
// tests/vu_lean_diff_tests.cpp runs every catalogue block against the
// interpreter from random state and compares all of that.

#include <mutex>
#include <set>
#include <unordered_set>

struct VU1Interpreter::Aot
{
    enum : uint32_t
    {
        kMaybeBranch = 1u, // the previous slot may have taken a branch
        kArmE = 2u,        // this slot's E bit ends the program after the next slot
        kLast = 4u,        // this slot leaves the pc behind for the dispatcher
        // Block analysis results (see the file comment); 0 = do the step.
        kNoMacWord = 8u,
        kNoRingWrite = 16u,
        kNoVisible = 32u,
        kCleanVs = 64u,
        kCleanVt = 128u,
        kCleanAcc = 256u
    };

    // The lower ops that read a flag register through the visibility ring
    // (execLowerT: FCEQ/FCAND/FCOR, FSEQ/FSAND/FSOR, FMEQ/FMAND/FMOR, FCGET).
    static constexpr bool readsFlags(uint32_t lower)
    {
        const uint32_t op = (lower >> 25) & 0x7Fu;
        return op == 0x10u || op == 0x12u || op == 0x13u || op == 0x14u || op == 0x16u ||
               op == 0x17u || op == 0x18u || op == 0x1Au || op == 0x1Bu || op == 0x1Cu;
    }

    static RRV_SIMD_INLINE int begin(VU1Interpreter &vu) { return vu.m_flagRingPos; }

    template <uint32_t N>
    static RRV_SIMD_INLINE void end(VU1Interpreter &vu, int pos)
    {
        vu.m_flagRingPos = (pos + static_cast<int>(N)) & 3;
    }

    // PC0 is the block's run-time entry pc: microprograms are relocatable (the
    // game uploads the same code at different addresses, and every VU branch
    // is pc-relative), so a block is keyed by its instruction words alone and
    // the pc is an argument, never a constant.
    template <uint32_t LOWER, uint32_t UPPER, uint32_t N, uint32_t FLAGS>
    static RRV_SIMD_INLINE void slot(VU1Interpreter &vu, int pos, uint32_t pc0)
    {
        using namespace rrv::vu;
        static constexpr SlotIR kIr = decodeSlotValue(LOWER, UPPER);
        constexpr bool kLoi = (kIr.ubits & UB_I) != 0u;
        // No lower op runs in this slot (an immediate, or a NOP).
        constexpr bool kNoLower = kLoi || kIr.lclass == LC_NOP;
        constexpr bool kUpperWritesVf0 =
            kIr.uop != U_NOP && ((kIr.udst == UD_FD && kIr.fd == 0u) || (kIr.udst == UD_FT && kIr.ft == 0u));
        constexpr bool kIsLast = (FLAGS & kLast) != 0u;
        // The generator's claims must agree with this file's decode of the slot.
        static_assert((FLAGS & kNoVisible) == 0u || kNoLower || !readsFlags(LOWER),
                      "kNoVisible on a slot whose lower op reads a flag");
        static_assert((FLAGS & kNoMacWord) == 0u || (kIr.uflags & UF_MAC) != 0u,
                      "kNoMacWord on a slot that is not an FMAC op");
        static_assert((FLAGS & (kCleanVs | kCleanVt | kCleanAcc)) == 0u || (kIr.uflags & UF_FLUSH) != 0u,
                      "operand-clean bits on an op that clamps no operand");
        static_assert(!kIsLast || (FLAGS & kNoRingWrite) == 0u,
                      "the last slot of a block always pushes its snapshot");
        constexpr unsigned kOpt = ((FLAGS & kNoMacWord) != 0u ? unsigned(kAotNoMacWord) : 0u) |
                                  ((FLAGS & kCleanVs) != 0u ? unsigned(kAotCleanVs) : 0u) |
                                  ((FLAGS & kCleanVt) != 0u ? unsigned(kAotCleanVt) : 0u) |
                                  ((FLAGS & kCleanAcc) != 0u ? unsigned(kAotCleanAcc) : 0u);
        VU1State &s = vu.m_state;
        const uint32_t pc = pc0 + 8u * N;

        bool take = false;
        uint32_t target = 0;
        if constexpr ((FLAGS & kMaybeBranch) != 0u)
        {
            take = s.branchPending;
            target = s.branchTarget;
            s.branchPending = false;
        }
        // Readers of the pc inside a slot: the lower pipe (branches, XGKICK's
        // packet tag) and the CLIP diagnostic. The last slot stores the next pc.
        if constexpr (!kNoLower || kIr.uop == U_CLIP)
            s.pc = pc;

        const int ring = (pos + static_cast<int>(N)) & 3;
        if constexpr ((FLAGS & kNoVisible) == 0u)
            vu.m_flagVisible = vu.m_flagRing[ring];

        if constexpr (kLoi)
        {
            const uint32_t bits = LOWER;
            std::memcpy(&s.i, &bits, 4);
        }

        if constexpr (kIr.uop != U_NOP)
            vu.template execUpperFastT<kIr.uop, kOpt>(kIr);

        if constexpr ((FLAGS & kNoRingWrite) == 0u)
            vu.m_flagRing[ring] = FlagSnapshot{s.mac, s.clip, s.status};

        if constexpr (!kLoi)
        {
            if constexpr (kIr.lclass == LC_NOP)
            {
                vu.m_updateMac = false;
            }
            else
            {
                constexpr int kOp = static_cast<int>((LOWER >> 25) & 0x7Fu);
                constexpr int kFunct = kOp == 0x40 ? static_cast<int>(LOWER & 0x3Fu) : -1;
                constexpr int kF2 = (kOp == 0x40 && kFunct >= 0x3C)
                                        ? static_cast<int>((LOWER & 0x3u) | ((LOWER >> 4) & 0x7Cu))
                                        : -1;
                vu.template execLowerT<kOp, kFunct, kF2>(LOWER, vu.m_curData, vu.m_curDataSize,
                                                         *vu.m_curGs, vu.m_curMemory, UPPER);
            }
        }

        // run() re-asserts VF0 and VI0 after every slot. Slot 0 does it for the
        // block; after that only a slot that could have written one of them.
        if constexpr (N == 0u || !kNoLower || kUpperWritesVf0)
        {
            static const float kVf0[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            rrv::simd::storeu(s.vf[0], rrv::simd::loadu(kVf0));
            s.vi[0] = 0;
        }

        if constexpr ((FLAGS & kLast) != 0u)
        {
            // run()'s fall-through, wrap included (the dispatcher only runs a
            // block that fits below codeSize, so only the last slot can wrap).
            uint32_t next = pc + 8u;
            if (next >= vu.m_aotCodeSize)
                next = 0u;
            if constexpr ((FLAGS & kMaybeBranch) != 0u)
                s.pc = take ? (target & 0x3FFFu) : next;
            else
                s.pc = next;
        }
        if constexpr ((FLAGS & kArmE) != 0u)
            s.ebit = true;
    }
};

namespace
{
#if __has_include("rrv_vu_aot_blocks.inc")
#include "rrv_vu_aot_blocks.inc"
#else
// No catalogue in this build: every lookup misses and the interpreter runs
// everything (and RRV_VU_AOT_RECORD can still collect the programs).
const VU1Interpreter::AotBlock *const kVuAotBlocks = nullptr;
constexpr size_t kVuAotBlockCount = 0;
constexpr const char kVuAotCatalogId[] = "none";
#endif

bool vuAotEnvOn(const char *name, bool dflt)
{
    const char *v = std::getenv(name);
    if (!v || !v[0])
        return dflt;
    return !(v[0] == '0' && v[1] == '\0');
}

uint64_t vuAotFnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i)
    {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}
} // namespace

#include "rrv_vu_prog_engine.inc"

void VU1Interpreter::aotEnsure()
{
    ++m_aotCalls;
    if ((m_aotCalls & 0xFFu) == 0u)
        ++m_aotGen; // expire the negative cache (see ps2_vu1.h)
    if (RRV_VU_UNLIKELY(m_aotStats) && (m_aotCalls & 0x3FFFu) == 0u)
        aotReportStats("periodic");
    if (m_aotInit)
        return;
    m_aotInit = true;
    m_aotEnabled = vuAotEnvOn("RRV_VU_AOT", true);
    m_aotVerify = vuAotEnvOn("RRV_VU_AOT_VERIFY", false);
    m_aotStats = vuAotEnvOn("RRV_VU_AOT_STATS", false);
    m_aotGenCheck = vuAotEnvOn("RRV_VU_AOT_GENCHECK", false);
    if (const char *d = std::getenv("RRV_VU_AOT_RECORD"); d && d[0])
        m_aotRecordDir = d;
    // Two execLower probes keep per-call-site state in function-local statics
    // and are not covered by m_cfgAnyDiag. Compiled blocks instantiate their
    // own copies of that state, so a run that arms them interprets.
    const char *splitProbe = nullptr;
    for (const char *name : {"RRV_P7_VU_TRACE", "RRV_TEX_ENTRY"})
        if (const char *v = std::getenv(name); v && v[0] && !(v[0] == '0' && v[1] == '\0'))
            splitProbe = name;
    if (splitProbe)
        m_aotEnabled = false;
    std::fprintf(stderr, "[vu-aot] %s for %s: catalogue %s, %zu blocks%s%s%s%s\n",
                 m_aotEnabled ? "enabled" : "disabled", m_isVu0 ? "vu0" : "vu1",
                 kVuAotCatalogId, kVuAotBlockCount,
                 m_aotVerify ? ", RRV_VU_AOT_VERIFY=1 (differential gate)" : "",
                 m_aotRecordDir ? ", recording misses" : "",
                 splitProbe ? ", interpreting because " : "", splitProbe ? splitProbe : "");
}

void VU1Interpreter::aotReportStats(const char *why)
{
    const uint64_t total = m_aotSlots + m_interpSlots;
    std::fprintf(stderr,
                 "[vu-aot] %s %s: %llu M slots compiled, %llu interpreted (%.4f%%), %llu misses",
                 m_isVu0 ? "vu0" : "vu1", why, (unsigned long long)(m_aotSlots >> 20),
                 (unsigned long long)m_interpSlots,
                 total ? 100.0 * (double)m_interpSlots / (double)total : 0.0,
                 (unsigned long long)m_aotMisses);
    if (m_aotGenCheck)
        std::fprintf(stderr, ", genstale %llu", (unsigned long long)m_aotGenStale);
    std::fputc('\n', stderr);
}

const VU1Interpreter::AotBlock *VU1Interpreter::aotLookup(uint32_t pc, const uint8_t *vuCode,
                                                          uint32_t codeSize)
{
    const uint32_t idx = pc >> 3;
    const uint32_t gen = g_rrvVuCodeGen[m_isVu0 ? 0 : 1].load(std::memory_order_relaxed);
    if (const AotBlock *b = m_aotByPc[idx])
    {
        const bool fits = pc + b->slots * 8u <= codeSize;
        // Gate 5: validated at this micro-memory generation -> no compare.
        if (fits && m_aotByPcGen[idx] == gen)
        {
            if (!RRV_VU_UNLIKELY(m_aotGenCheck))
                return b;
            if (std::memcmp(vuCode + pc, b->raw, b->slots * 8u) == 0)
                return b;
            ++m_aotGenStale; // a micro-memory writer did not bump the generation
        }
        else if (fits && std::memcmp(vuCode + pc, b->raw, b->slots * 8u) == 0)
        {
            m_aotByPcGen[idx] = gen;
            return b;
        }
        m_aotByPc[idx] = nullptr; // the program under this pc changed
        m_aotByPcGen[idx] = 0u;
    }
    uint64_t first;
    std::memcpy(&first, vuCode + pc, sizeof(first));
    if (m_aotMissGen[idx] == m_aotGen && m_aotMissWord[idx] == first)
        return nullptr;

    // Sorted by (first slot word, slots descending): prefer the longest match
    // that fits below codeSize.
    const AotBlock *lo = kVuAotBlocks;
    size_t n = kVuAotBlockCount;
    while (n > 0)
    {
        const size_t half = n / 2;
        const AotBlock *mid = lo + half;
        if (mid->first < first)
        {
            lo = mid + 1;
            n -= half + 1;
        }
        else
        {
            n = half;
        }
    }
    const AotBlock *const endp = kVuAotBlocks + kVuAotBlockCount;
    for (; lo != endp && lo->first == first; ++lo)
    {
        if (pc + lo->slots * 8u <= codeSize &&
            std::memcmp(vuCode + pc, lo->raw, lo->slots * 8u) == 0)
        {
            m_aotByPc[idx] = lo;
            m_aotByPcGen[idx] = gen;
            return lo;
        }
    }
    m_aotMissGen[idx] = m_aotGen;
    m_aotMissWord[idx] = first;
    ++m_aotMisses;
    if (RRV_VU_UNLIKELY(m_aotRecordDir != nullptr))
        aotRecordMiss(pc, vuCode, codeSize);
    return nullptr;
}

// RRV_VU_AOT_RECORD: the program image (game data: the directory must be under
// local/) and every pc that missed in it, for tools/vu-aot/vu_aot_gen.py.
void VU1Interpreter::aotRecordMiss(uint32_t pc, const uint8_t *vuCode, uint32_t codeSize)
{
    static std::mutex mutex;
    static std::unordered_set<uint64_t> images;
    static std::set<std::pair<uint64_t, uint32_t>> seen;
    const std::lock_guard<std::mutex> lock(mutex);
    const uint64_t hash = vuAotFnv1a(vuCode, codeSize);
    if (!seen.insert({hash, pc}).second)
        return;
    char base[1024];
    std::snprintf(base, sizeof(base), "%s/vu%d-%05u-%016llx", m_aotRecordDir, m_isVu0 ? 0 : 1,
                  codeSize, (unsigned long long)hash);
    char path[1100];
    if (images.insert(hash).second)
    {
        std::snprintf(path, sizeof(path), "%s.bin", base);
        if (FILE *f = std::fopen(path, "rb"))
            std::fclose(f);
        else if (FILE *w = std::fopen(path, "wb"))
        {
            std::fwrite(vuCode, 1, codeSize, w);
            std::fclose(w);
        }
    }
    std::snprintf(path, sizeof(path), "%s.pcs", base);
    if (FILE *f = std::fopen(path, "a"))
    {
        std::fprintf(f, "%04x\n", pc);
        std::fclose(f);
    }
}

// Runs compiled blocks from state.pc for at most `maxCycles` slots. Returns
// true when a block ended the microprogram (E bit); otherwise the caller
// interprets from state.pc.
bool VU1Interpreter::aotRun(const uint8_t *vuCode, uint32_t codeSize, uint32_t maxCycles,
                            uint32_t &slotsExecuted)
{
    uint32_t executed = 0;
    m_aotCodeSize = codeSize;
    while (executed < maxCycles)
    {
        // Entry states a block is not built for: a pending branch (this slot
        // is a delay slot) and a pending E bit (this slot is the last one).
        if (m_state.branchPending || m_state.ebit)
            break;
        const uint32_t pc = m_state.pc;
        if ((pc & 7u) != 0u || pc + 8u > codeSize || (pc >> 3) >= kIrSlots)
            break;
        const AotBlock *b = aotLookup(pc, vuCode, codeSize);
        if (!b || executed + b->slots > maxCycles)
            break;
        b->fn(*this, pc);
        executed += b->slots;
        if (b->stop)
        {
            slotsExecuted = executed;
            return true;
        }
    }
    slotsExecuted = executed;
    return false;
}

// Compiled blocks bake these settings at their defaults (scripts/
// vu_aot_overlay.py, UPPER_CFG / LOWER_CFG / applyDestVT); any other value
// sends the run to the interpreter, which reads them at run time.
bool VU1Interpreter::aotConfigIsDefault() const
{
    return !m_cfgDenorm && vuOpClampEnabled() && m_cfgMacExact && m_cfgPipeline &&
           !m_cfgAnyDiag && !m_cfgClipDiag && !vu0FmandDiagEnabled() && !vuDivExactEnabled() &&
           vuEatanMicroVu() && !g_vu1Diag.enabled;
}

// ---- Lean entry for both units (RRV_VU0_LEAN, RRV_VU1_LEAN; default on) ------
//
// VU0: RR5 calls micro mode (VCALLMS) once per vertex from its geometry
// builders; a typical call runs only a few dozen slots. Measured on v140
// (host `sample`, demo race), the call path around the compiled blocks cost
// more than the blocks: execute() and run() re-check every diagnostic switch,
// re-seed state no VU0 run uses and open the cadence scope, on every call.
// VU1: the same execute() + run() preamble runs once per MSCAL (thousands per
// field); the Steam Deck profile has the rrv-vu1 thread, which is the first
// limiter there, spending about 14% in run()/aotLookup.
//
// This is execute() + run() with every switch they test resolved once to its
// default. It performs the same state writes in the same order and runs the
// same compiled blocks. Where no block matches (or a block leaves a delay slot
// or an E-bit slot pending), the unchanged run() takes over from that exact
// state with the remaining slot budget (m_aotLeanResume keeps the pending
// branch). Returns false, having changed nothing, when any switch is not at
// its default: the caller then takes the full path. RRV_VU0_LEAN_OFF=1 and
// RRV_VU1_LEAN_OFF=1 are the per-unit controls; tests/vu_lean_diff_tests.cpp
// compares this entry with execute() on real catalogue instruction sequences.
bool VU1Interpreter::aotLean(uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData,
                             uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t startPC,
                             uint32_t itop, uint32_t maxCycles, uint32_t top)
{
    // Every input of execute()'s and run()'s per-call decisions except the
    // RRV_VU_AOT ones, which aotEnsure() resolves on its first call.
    static const bool s_cfgOk = [] {
        // RRV_P7_FORCE_VF3Z is read by execute() for VU1 only; it patches a register.
        const char *probes[] = {"RRV_VU_STEPTRACE", "RRV_VU_INTRACE", "RRV_P7_VU_TRACE",
                                "RRV_VU_MATRIX_TRACE", "RRV_TEX_ENTRY", "RRV_CADENCE_DIAG",
                                "RRV_P7_FORCE_VF3Z"};
        for (const char *name : probes)
            if (const char *v = std::getenv(name); v && v[0] && !(v[0] == '0' && v[1] == '\0'))
                return false;
        vu1DiagInit();
        return !g_vu1Diag.enabled && vuFastPathEnabled() && !vuVerifyEnabled() &&
               vuMacExactEnabled() && !vuDenormFlushEnabled() && vuFlagPipelineEnabled() &&
               vuClipDiagLimit() == 0u && !vuQLatencyEnabled() && !vu0FmandDiagEnabled() &&
               vuOpClampEnabled() && !vuDivExactEnabled() && vuEatanMicroVu();
    }();
    static const bool s_vu0Off = vuAotEnvOn("RRV_VU0_LEAN_OFF", false);
    static const bool s_vu1Off = vuAotEnvOn("RRV_VU1_LEAN_OFF", false);
    if (!s_cfgOk || (m_isVu0 ? s_vu0Off : s_vu1Off))
        return false;
    aotEnsure();
    if (!m_aotEnabled || m_aotVerify || m_aotForceInterp)
        return false;

    // execute()
    const ScopedVuRounding vuRounding(true);
    m_state.pc = startPC;
    m_entryPC = startPC;
    m_state.ebit = false;
    m_state.itop = itop;
    m_state.top = top;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    {
        const FlagSnapshot seed{m_state.mac, m_state.clip, m_state.status};
        for (auto &slot : m_flagRing)
            slot = seed;
        m_flagRingPos = 0;
        m_flagVisible = seed;
    }

    // run(), up to the first slot no compiled block covers
    m_state.branchPending = false;
    m_codeImage = vuCode;
    m_codeSize = codeSize;
    m_cfgFast = true;
    m_cfgVerify = false;
    m_cfgMacExact = true;
    m_cfgDenorm = false;
    m_cfgFlagRing = true;
    m_cfgPipeline = true;
    m_cfgClipDiag = false;
    m_cfgQLatency = false;
    m_qStageActive = false;
    m_cfgAnyDiag = false;
    m_curData = vuData;
    m_curDataSize = dataSize;
    m_curGs = &gs;
    m_curMemory = memory;

    // Whole-program native microprograms (rrv_vu_prog_engine.inc): the program
    // for this entry runs first; the compiled blocks take whatever it leaves.
    uint32_t ran = 0;
    bool ended = false;
    m_aotCodeSize = codeSize;
    progEnsure();
    if (!m_isVu0)
        vuProgCapture(m_state, vuCode, codeSize, vuData, dataSize, startPC, itop, top);
    if (m_progEnabled)
    {
        if (const ProgEntry *prog = progLookup(startPC, vuCode, codeSize))
        {
            if (RRV_VU_UNLIKELY(m_progVerify))
            {
                progVerified(prog, vuCode, codeSize, vuData, dataSize, startPC, maxCycles, ran);
                ++m_progCalls;
                m_aotSlots += ran;
                return true;
            }
            uint32_t progEnded = 0;
            ran = prog->fn(*this, vuData, dataSize - 1u, startPC - prog->entry, maxCycles, progEnded);
            ended = progEnded != 0u;
            ++m_progCalls;
            m_progSlots += ran;
            if (!ended)
                ++m_progHandOvers;
        }
        else
        {
            ++m_progNoProgram;
        }
    }
    if (!ended)
    {
        uint32_t more = 0;
        ended = aotRun(vuCode, codeSize, maxCycles - ran, more);
        ran += more;
    }
    m_aotSlots += ran;
    if (!ended && ran < maxCycles && m_state.pc + 8u <= codeSize)
    {
        m_aotLeanResume = true;
        run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles - ran);
        m_aotLeanResume = false;
        return true;
    }
    qFlush();
    return true;
}
