#ifndef PS2_VU1_H
#define PS2_VU1_H

#include <cstdint>
#include <memory>
#include <vector>

#include "runtime/rrv_simd.h"
#include "runtime/rrv_vu_ir.h"

class GS;
class PS2Memory;

namespace ps2_diag
{
    struct CarDmaVu1Result
    {
        uint32_t attempted = 0;
        uint32_t submitted = 0;
        uint32_t skippedInvalid = 0;
        uint32_t skippedOverflow = 0;
        uint32_t skippedEmpty = 0;
    };

    void beginCarDmaVu1Scope(uint64_t chainId, uint32_t vifByteOffset);
    CarDmaVu1Result endCarDmaVu1Scope();
}

struct VU1State
{
    float vf[32][4];
    int32_t vi[16];
    float acc[4];
    float q;
    float p;
    float i;
    uint32_t pc;
    uint32_t mac;
    uint32_t clip;
    uint32_t status;
    bool ebit;
    uint32_t itop;
    uint32_t xitop;
    uint32_t top;   // VIF1 TOP (double-buffer output base) — returned by XTOP
    // VU branches have ONE delay slot: the instruction after the branch always
    // executes before control transfers. A taken branch records its target here;
    // run() applies it only after executing the following (delay-slot) instruction.
    bool branchPending;
    uint32_t branchTarget;
};

class VU1Interpreter
{
public:
    VU1Interpreter();
    // Out of line (ps2_vu1.cpp).
    ~VU1Interpreter();

    void reset();

    void execute(uint8_t *vuCode, uint32_t codeSize,
                 uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory = nullptr,
                 uint32_t startPC = 0, uint32_t itop = 0,
                 uint32_t maxCycles = 65536, uint32_t top = 0);

    void resume(uint8_t *vuCode, uint32_t codeSize,
                uint8_t *vuData, uint32_t dataSize,
                GS &gs, PS2Memory *memory = nullptr,
                uint32_t itop = 0, uint32_t maxCycles = 65536,
                uint32_t top = 0);

    VU1State &state() { return m_state; }
    const VU1State &state() const { return m_state; }

    // B-5: distinguishes PS2Runtime's m_vu0 instance (VU0 micro-mode, invoked
    // from executeVU0Microprogram) from m_vu1, for the RRV_VU0_FMAND_DIAG probe
    // and any future VU0-only diagnostics. Set once at construction; not part
    // of architectural state.
    void setIsVu0(bool isVu0) { m_isVu0 = isVu0; }
    bool isVu0() const { return m_isVu0; }

    // RRV_VU0_LEAN / RRV_VU1_LEAN: execute() for either unit with every per-call
    // switch resolved once (rrv_vu_aot_engine.inc). False = not eligible, nothing
    // changed; the caller runs execute().
    bool aotLean(uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory, uint32_t startPC, uint32_t itop,
                 uint32_t maxCycles, uint32_t top);

    // Statically recompiled microcode (RRV_VU_AOT, src/vu-aot/). Aot holds the
    // per-slot sequence generated blocks are written in; it is a member so it
    // may use the interpreter's private state and op handlers. AotBlock is one
    // catalogue entry: `raw` is the `slots` 64-bit words the block was
    // generated from, and the block runs, at whatever pc, only when micro
    // memory there matches them.
    struct Aot;
    struct AotBlock
    {
        uint64_t first;    // raw[0], the lookup key
        uint32_t slots;
        uint32_t stop;     // the block ends the microprogram (E bit)
        const uint64_t *raw;
        void (*fn)(VU1Interpreter &, uint32_t pc); // position independent
    };

    // ---- whole-program native microprograms (RRV_VU_PROG, src/vu-aot/) -------
    //
    // A program is one native function for one entry of one microprogram image
    // (generated/rr5/vu/rrv_vu_aot_programs.inc, tools/vu-aot/vu_prog_gen.py):
    // the same slots the compiled blocks run, with the registers in host
    // registers for the whole run, the MAC word computed only where something
    // reads it and no dispatch between blocks. It runs only when micro memory
    // holds exactly the words it was generated from, and hands back to the
    // compiled blocks (with the interpreter's exact state) at anything it does
    // not cover. `RRV_VU_PROG_VERIFY=1` runs every invocation both ways from
    // one snapshot and compares VU1State, the data memory and every XGKICK.
    struct Prog;
    struct ProgRange
    {
        uint32_t offset; // bytes from the program's lowest slot
        uint32_t slots;
    };
    struct ProgEntry
    {
        uint32_t entry;          // bytes from the program's lowest slot to its entry slot
        uint64_t first;          // the entry slot's words (fast reject)
        const uint64_t *raw;     // the image the program was generated from, at its lowest slot
        uint32_t imageSlots;     // slots of the image from there on
        const ProgRange *ranges; // the slots the program can reach
        uint32_t rangeCount;
        uint32_t id;
        uint32_t segments;       // labels in the function (test coverage)
        // Runs from `base + entry` (`base`: where the lowest slot is loaded);
        // returns the slots executed. `ended` is 1 when the program ended (E
        // bit), 0 when it handed over at m_state.pc.
        uint32_t (*fn)(VU1Interpreter &, uint8_t *vuData, uint32_t dataMask, uint32_t base,
                       uint32_t maxSlots, uint32_t &ended);
    };

private:
    VU1State m_state;

    // Set true by execUpper for FMAC-arithmetic ops; applyDest consumes it to
    // refresh the MAC flags (Z/S per component) from the result. Cleared for
    // MAX/MINI/ITOF/FTOI/move and the whole lower pipe, which don't touch flags.
    bool m_updateMac = false;

    // RRV_VU_FLAG_PIPELINE (B-5): real VU hardware makes an upper op's MAC/
    // status/clip flags visible to lower-pipe flag readers (FMAND/FSAND/FCAND
    // etc.) only ~4 issue slots later; m_state.mac/status/clip above are
    // updated instantly instead. This ring records a {mac,status,clip}
    // snapshot once per executed instruction pair (after the upper executes,
    // before the lower runs); run() captures the snapshot from 4 slots ago
    // into m_flagVisible before overwriting that slot, and lower-pipe flag
    // readers consult m_flagVisible instead of the live m_state fields when
    // the pipeline model is enabled. See docs/HANDOFF_BOOT_NAMCO_WHITE.md.
    // Padded to 16 bytes and 16-byte aligned so an entry moves with one
    // 128-bit load/store.
    // Member ORDER matches VU1State's mac/clip/status, which are adjacent: that
    // is what lets a compiled block push a whole snapshot with one 128-bit load
    // from m_state and one store, instead of three of each. Every use is by
    // name, so the order is layout only.
    struct alignas(16) FlagSnapshot
    {
        uint32_t mac = 0;
        uint32_t clip = 0;
        uint32_t status = 0;
        uint32_t pad = 0;
    };
    // DIAG-TEMP (T-P7-CLIPWHO, active only under RRV_VU_CLIP_DIAG): the CLIP
    // register as the OTHER form would have built it, carried through the SAME
    // 4-slot visibility ring, so a reader can be shown both answers at the same
    // instruction. m_clipWatch is a countdown armed by a disagreeing CLIP.
    uint32_t m_clipShadow = 0;
    uint32_t m_clipShadowRing[4] = {0, 0, 0, 0};
    uint32_t m_clipShadowVisible = 0;
    uint32_t m_clipWatch = 0;
    uint32_t m_clipWatchPc = 0;
    uint8_t *m_codeImage = nullptr;   // current microprogram, for the CLIP dump
    uint32_t m_codeSize = 0;
    FlagSnapshot m_flagRing[4];
    int m_flagRingPos = 0;
    FlagSnapshot m_flagVisible; // this iteration's 4-slots-ago snapshot (run())
    bool m_isVu0 = false;       // true only for PS2Runtime::m_vu0
    // Diagnostic only (RRV_VU0_FMAND_DIAG): the startPC this execute() call was
    // launched with, so the probe can tell which VCALLMS entry currently owns
    // VU0 micro memory — later entries (e.g. the main-game program) can reuse
    // the SAME byte offsets as the white-Namco entry (0x20) once VU0 code
    // memory is re-uploaded, so pc==0x2D0 alone is not sufficient to identify
    // the white payload's FMEQ.
    uint32_t m_entryPC = 0;

    // ---- per-run configuration -------------------------------------------
    // Resolved once per run() call from the getenv-cached statics, so the hot
    // loop reads a member instead of re-entering a function-local-static guard
    // on every instruction.
    bool m_cfgFast = true;       // RRV_VU_FASTPATH
    bool m_cfgVerify = false;    // RRV_VU_VERIFY
    bool m_cfgMacExact = true;   // RRV_VU_MAC_EXACT
    bool m_cfgDenorm = false;    // RRV_VU_DENORM
    bool m_cfgFlagRing = true;   // RRV_VU_FLAG_PIPELINE || RRV_VU0_FMAND_DIAG
    bool m_cfgPipeline = true;   // RRV_VU_FLAG_PIPELINE alone (execLower reads it)
    bool m_cfgClipDiag = false;  // RRV_VU_CLIP_DIAG
    bool m_cfgAnyDiag = false;   // any per-instruction diagnostic is armed
    bool m_cfgQLatency = false;  // RRV_VU_Q_LATENCY (opt-in)

    // ---- FDIV (Q) pipeline ------------------------------------------------
    // Provenance: PCSX2 d5f75c9e4, pcsx2/VUops.cpp — _vuFDIVAdd/_vuFDIVflush/
    // _vuFlushAll (GPL-3.0, same licence as this tree).  DIV/SQRT take 7 VU
    // cycles and RSQRT 13 before their result becomes VISIBLE in Q; the
    // register keeps its previous value until then.  Writing Q at the DIV, as
    // this file did before, is only equivalent for microprograms that do not
    // software-pipeline the perspective divide.
    //
    // MEASURED NON-CAUSAL for the phase-39 race defect (docs/KNOWN_ISSUES.md
    // item 17): identical 1.1% on-screen-vertex fraction with and without it.
    // Kept as an opt-in because the semantics are right and the next
    // Q-dependent divergence should not have to rediscover them; see
    // ps2_vu1.cpp's vuQLatencyEnabled() for why it defaults OFF.
    bool m_qStageActive = false;
    float m_qStage = 0.0f;
    uint32_t m_qStageReadyCycle = 0;   // cycle at which m_qStage becomes visible
    uint32_t m_curCycle = 0;           // run()'s loop counter, for the above

    // Stage a DIV/SQRT/RSQRT result.  `latency` is in VU cycles (7 or 13).
    void qWrite(float value, uint32_t latency);
    // Commit any staged Q immediately (WAITQ, and end of microprogram).
    void qFlush();
    // Per-instruction pipe test: commit once the latency has elapsed.
    void qTick();

    // ---- pre-decoded microprogram (rrv_vu_ir.h) ---------------------------
    // One entry per 8-byte instruction slot of VU micro memory (VU1: 16 KB =
    // 2048 slots; VU0 uses the first 512).  Each entry re-validates against the
    // raw 64 bits it was decoded from, so an MPG upload that replaces the
    // program can never execute a stale decode — no invalidation protocol, no
    // epoch counter.  Compiled blocks (RRV_VU_AOT) use the same decoder.
    static constexpr uint32_t kIrSlots = 2048;
    rrv::vu::SlotIR m_ir[kIrSlots] = {};
    uint64_t m_verifyMismatches = 0;
    uint64_t m_verifyChecked = 0;

    void run(uint8_t *vuCode, uint32_t codeSize,
             uint8_t *vuData, uint32_t dataSize,
             GS &gs, PS2Memory *memory, uint32_t maxCycles);

    // ---- statically recompiled microcode (RRV_VU_AOT, default ON) ----------
    //
    // Generated ahead of time from the game's microprograms (generated/rr5/vu,
    // scripts/vu_aot_overlay.py); there is no run-time code generation. Blocks
    // call execUpperFastT/execLowerT with constant fields, i.e. the same source
    // the interpreter runs, and are dispatched only when micro memory holds
    // exactly the words they were generated from.
    //
    // `RRV_VU_AOT_VERIFY=1` is the acceptance gate: every microprogram
    // invocation is executed TWICE from an identical snapshot — once by the
    // interpreter (whose results the game keeps) and once through the compiled
    // blocks with GIF side effects suppressed — and the final VU1State and the
    // whole of VU data memory are compared.
    bool m_aotInit = false;
    bool m_aotEnabled = false;
    bool m_aotVerify = false;
    bool m_aotStats = false;
    const char *m_aotRecordDir = nullptr;
    // Side-effect suppression for the verification pass. XGKICK is the only
    // lower op that leaves the VU: it submits a GIF packet and touches no
    // architectural state, so skipping it makes the second pass observable
    // but harmless.
    bool m_suppressSideEffects = false;
    // The current run()'s arguments, for the compiled blocks' lower pipe.
    uint8_t *m_curData = nullptr;
    uint32_t m_curDataSize = 0;
    GS *m_curGs = nullptr;
    PS2Memory *m_curMemory = nullptr;
    uint64_t m_aotSlots = 0;    // slots executed by compiled blocks
    uint64_t m_interpSlots = 0; // slots the interpreter ran instead
    uint64_t m_aotMisses = 0;   // lookups that found no block
    uint64_t m_aotCalls = 0;    // execute()/resume() calls, for the stats line
    bool m_aotForceInterp = false; // pass 1 of the differential gate
    // Per-pc block cache (validated by memcmp on every dispatch) and a negative
    // cache of misses: the first slot word seen at a missed pc, valid for one
    // generation (bumped every 256 execute() calls, so code that appears later
    // under the same first word is found again).
    const AotBlock *m_aotByPc[kIrSlots] = {};
    // Gate 5: g_rrvVuCodeGen value at which m_aotByPc[i] was last validated
    // (0 = never); a block validated at the current generation skips the
    // memcmp. RRV_VU_AOT_GENCHECK=1 compares anyway and counts disagreements.
    uint32_t m_aotByPcGen[kIrSlots] = {};
    bool m_aotGenCheck = false;
    uint64_t m_aotGenStale = 0;
    uint64_t m_aotMissWord[kIrSlots] = {};
    uint32_t m_aotMissGen[kIrSlots] = {};
    uint32_t m_aotGen = 1;
    uint32_t m_aotCodeSize = 0; // the running program's micro memory size
    void aotEnsure();
    bool aotRun(const uint8_t *vuCode, uint32_t codeSize, uint32_t maxCycles,
                uint32_t &slotsExecuted);
    const AotBlock *aotLookup(uint32_t pc, const uint8_t *vuCode, uint32_t codeSize);
    void aotRecordMiss(uint32_t pc, const uint8_t *vuCode, uint32_t codeSize);
    void aotReportStats(const char *why);
    // The configuration compiled blocks bake is the one in effect.
    bool aotConfigIsDefault() const;
    // ---- whole-program native microprograms: state (types above) --------------
    bool m_progInit = false;
    bool m_progEnabled = false;
    bool m_progVerify = false;
    const ProgEntry *m_progByPc[kIrSlots] = {};
    uint32_t m_progByPcGen[kIrSlots] = {}; // g_rrvVuCodeGen at the last lookup (0 = never)
    uint64_t m_progCalls = 0;     // invocations a program took
    uint64_t m_progSlots = 0;     // slots they executed
    uint64_t m_progHandOvers = 0; // invocations that handed over to the blocks
    uint64_t m_progNoProgram = 0; // invocations with no matching program
    // RRV_VU_PROG_VERIFY: every XGKICK of the two passes (pc, address, data-memory hash).
    std::vector<uint64_t> *m_kickLog = nullptr;
    void progEnsure();
    const ProgEntry *progLookup(uint32_t pc, const uint8_t *vuCode, uint32_t codeSize);
    bool progVerified(const ProgEntry *prog, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData,
                      uint32_t dataSize, uint32_t startPC, uint32_t maxCycles, uint32_t &ran);
    void progReport(const char *why);

    // Set only around aotLean()'s hand-over to run(), so run() keeps a pending
    // branch. Per instance: VU0 (EE thread) and VU1 (owner thread) hand over
    // concurrently, so this must not be shared between them.
    bool m_aotLeanResume = false;
    // RRV_VU_AOT_VERIFY: run this invocation twice from one snapshot and
    // compare everything the VU can observe.
    void runVerified(uint8_t *vuCode, uint32_t codeSize,
                     uint8_t *vuData, uint32_t dataSize,
                     GS &gs, PS2Memory *memory, uint32_t maxCycles);

    void execUpper(uint32_t instr);
    void execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr);
    // execLower() with the primary opcode, the special funct and the special2
    // funct as template constants (-1: decoded at run time, which is what
    // execLower() runs). RRV_VU_AOT blocks call the constant instances.
    template <int KOP, int KFUNCT, int KF2>
    void execLowerT(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr);

    // ---- optimised upper pipe (RRV_VU_FASTPATH, default on) ---------------
    // Same semantics as execUpper(), driven by the pre-decoded slot IR and
    // computed 4 lanes at a time through rrv::simd.  execUpper() above stays
    // the reference implementation: RRV_VU_FASTPATH=0 runs it instead, and
    // RRV_VU_VERIFY=1 runs BOTH on every instruction and compares the
    // architectural state (docs/TESTING.md T-VU-FASTPATH).
    void execUpperFast(const rrv::vu::SlotIR &ir);
    // The same body with the opcode as a template constant (-1: read ir.uop at
    // run time, which is what execUpperFast() runs). RRV_VU_AOT blocks call
    // the constant instances.
    //
    // KOPT (AotOpt bits) is what a compiled block knows about this slot from
    // the rest of its block (tools/vu-aot/vu_aot_gen.py, "Block analysis"):
    // the MAC word is overwritten before anything can read it, or an operand's
    // lanes were written by an FMAC op earlier in the block and therefore
    // cannot hold an exponent of 255, so the operand clamp is the identity.
    // 0 (the interpreter, and any block generated without the analysis)
    // computes everything.
    enum AotOpt : unsigned
    {
        kAotNoMacWord = 1u, // skip the MAC word; the stored value is still sanitised
        kAotCleanVs = 2u,   // no operand clamp on vf[fs]
        kAotCleanVt = 4u,   // no operand clamp on vf[ft]
        kAotCleanAcc = 8u   // no operand clamp on ACC
    };
    template <int KUOP, unsigned KOPT = 0u>
    void execUpperFastT(const rrv::vu::SlotIR &ir);
    // Picks the upper-pipe implementation for one slot: the fast path, the
    // scalar reference, or (RRV_VU_VERIFY) both plus a comparison.
    void execUpperSlot(const rrv::vu::SlotIR &ir, uint32_t upper);
    // Masked write-back + MAC update, branchless.  `updatesMac` comes from the
    // IR (UF_MAC) instead of the m_updateMac carry the scalar path uses.
    void applyDestV(float *dst, rrv::simd::f32x4 result, uint8_t dest, bool updatesMac);
    // KCFG >= 0: the RRV_VU_AOT instance, with the default configuration baked.
    template <int KCFG, unsigned KOPT = 0u>
    void applyDestVT(float *dst, rrv::simd::f32x4 result, uint8_t dest, bool updatesMac);
    // PCSX2 VU_MAC_UPDATE, four lanes at once; returns the value the VU stores.
    rrv::simd::f32x4 macUpdateV(rrv::simd::f32x4 result, uint8_t dest);
    // The same with the MAC word optional: KWORD = false returns the stored
    // value and leaves m_state.mac alone (kAotNoMacWord).
    template <bool KWORD>
    rrv::simd::f32x4 macUpdateVT(rrv::simd::f32x4 result, uint8_t dest);
    // Legacy Z/S-only MAC flags (RRV_VU_MAC_EXACT=0), four lanes at once.
    void setMacFlagsV(rrv::simd::f32x4 result, uint8_t dest);
    // The CLIP op, shared verbatim by execUpper() and execUpperFast() so the
    // most correctness-sensitive op in the file has exactly one implementation.
    void clipOp(const float *vs, const float *vt);
    // RRV_VU_VERIFY: report the first N slots where the two paths disagree.
    void reportFastPathMismatch(const rrv::vu::SlotIR &ir, const VU1State &expected);

    void applyDest(float *dst, const float *result, uint8_t dest);
    void applyDestAcc(const float *result, uint8_t dest);
    void applyDestNoMac(float *dst, const float *result, uint8_t dest);
    void setMacFlags(const float *result, uint8_t dest);
    // PCSX2 VU_MAC_UPDATE (VUflags.cpp): sets the full O/U/S/Z MAC word and
    // writes into `out` the value the VU actually STORES -- a denormal result
    // becomes signed zero and an exp==255 result is clamped to +/-0x7f7fffff.
    // The VU has no infinities. Gated by RRV_VU_MAC_EXACT (default on).
    void macUpdate(float *out, const float *result, uint8_t dest);
    // DIAG-TEMP (T-P7-CLIPWHO): report the first clip reader consuming a
    // disagreeing CLIP, and what it answers under both forms.
    void clipWho(const char *kind, uint32_t imm24, uint32_t sel, uint32_t shadow,
                 uint32_t (*fn)(uint32_t, uint32_t));
    float broadcast(const float *vf, uint8_t bc);
};

#endif
