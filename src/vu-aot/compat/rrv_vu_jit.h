#ifndef RRV_VU_JIT_H
#define RRV_VU_JIT_H

// rrv_vu_jit.h — "no JIT" stand-in for the pinned producer's VU hook.
//
// The ARM64 VU JIT (src/vu-jit, vixl, MAP_JIT) was removed on 2026-09-30:
// the product runs statically recompiled VU microcode instead
// (scripts/vu_aot_overlay.py, src/vu-aot/rrv_vu_aot_engine.inc,
// generated/rr5/vu/). The pinned producer's ps2_vu1.cpp
// (build-deps/ps2recomp-d52-compatible-v2) still includes this header and asks
// for an engine; the development and test graphs that compile that file as-is
// get this interface, which never creates one, so they interpret every VU
// slot. Nothing here generates code at run time.

#include <cstdint>
#include <memory>

#include "runtime/ps2_vu1.h"   // VU1State
#include "runtime/rrv_vu_ir.h" // rrv::vu::SlotIR

namespace rrv::vujit
{

// Layout the producer's FlagSnapshot static_asserts against.
struct alignas(16) FlagSnapshotABI
{
    uint32_t mac;
    uint32_t clip;
    uint32_t status;
    uint32_t pad;
};

// The fields the producer's jitEnsure() fills before asking for an engine.
struct Env
{
    VU1State *state = nullptr;
    const rrv::vu::SlotIR *ir = nullptr;
    void *self = nullptr;
    void (*upperFn)(void *self, const rrv::vu::SlotIR *ir) = nullptr;
    void (*lowerFn)(void *self, uint32_t instr) = nullptr;
    FlagSnapshotABI *flagRing = nullptr;
    int32_t *flagRingPos = nullptr;
    FlagSnapshotABI *flagVisible = nullptr;
    uint8_t **dataPtr = nullptr;
    uint32_t *dataSizePtr = nullptr;
    bool flagRingActive = true;
    bool macExact = true;
    bool denormFlush = false;
    bool flagPipeline = true;
    bool clipExact = true;
    bool divExact = false;
    bool opClamp = false;
};

class Engine
{
public:
    virtual ~Engine() = default;
    virtual bool run(const uint8_t *code, uint32_t codeSize, uint32_t maxCycles,
                     uint32_t &slotsExecuted) = 0;
    virtual void stats(uint64_t &blocksCompiled, uint64_t &slotsCompiled,
                       uint64_t &slotsNative, uint64_t &slotsThunked) const = 0;
    // There is no run-time code generation: never an engine.
    static std::unique_ptr<Engine> create(const Env &) { return nullptr; }
};

inline bool compiledIn() { return false; }
inline bool enabledByEnv() { return false; }

} // namespace rrv::vujit

#endif // RRV_VU_JIT_H
