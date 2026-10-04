// Diagnostic-only GAME-001 control-flow receipt.  This header is consumed by
// the generated producer overlay; it intentionally has no guest-write hooks.
#ifndef RRV_M2P_GAME001_FAIL_CLOSED_H
#define RRV_M2P_GAME001_FAIL_CLOSED_H

#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace rrv::m2pgame001 {

// Called at the dispatch safepoint before lookupFunction() can select the
// producer's default/fallback body.  Returns false after retaining a bounded
// receipt and requesting a stop for an unregistered target.
bool beforeDispatch(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx,
                    uint32_t target) noexcept;

// Exact insertion points in the ignored generated owner: immediately after
// 0x258670, at the 0x258988 restore, and at the 0x2589b0 source selection.
// These routines observe values only; they do not watch guest writes.
void recordSavedRaStore(const R5900Context *ctx, uint32_t frameSp, uint32_t incomingRa,
                        uint64_t savedValue) noexcept;
void recordSavedRaRestore(const R5900Context *ctx, uint32_t frameSp, uint64_t savedValue,
                          uint32_t restoredRa) noexcept;
void recordFinalOperand(const R5900Context *ctx, uint32_t frameSp, uint32_t finalOperand) noexcept;

void recordChildAllocation(const R5900Context *ctx, uint32_t frameSp, uint32_t ra) noexcept;
void recordChildYield(const uint8_t *rdram, const R5900Context *ctx, uint32_t source,
                      uint32_t sp, uint32_t ra, uint32_t nextPc) noexcept;
void recordChildDispatch(const uint8_t *rdram, const R5900Context *ctx, uint32_t pc,
                         uint32_t sp, uint32_t ra) noexcept;
void recordChildReturn(const uint8_t *rdram, const R5900Context *ctx, uint32_t postSp,
                       uint32_t ra, uint32_t target) noexcept;
// Called after the actual 2d2c44 SD. Only overlaps with a published active child
// slot are retained; publication is atomic across host threads and contexts.
void recordChildSlotOverlap(const uint8_t *rdram, const R5900Context *ctx,
                            uint32_t sp, uint32_t ra, uint64_t writtenValue) noexcept;

// Exact insertion points in the ignored sub_00266F40 owner.  These retain the
// smallest relation needed to distinguish in-owner fall-through from a
// preempted dispatcher re-entry.  The overlay supplies register values, so the
// lifecycle probe can read only a tracked active saved-RA slot at generated
// boundaries. Dispatcher samples never read that slot. No hook writes guest state.
void recordOwnerPrimaryEntry(const R5900Context *ctx, uint32_t preSwitchPc,
                             uint32_t sp, uint32_t ra) noexcept;
void recordOwnerPreempt268b38(const R5900Context *ctx, uint32_t sp,
                              uint32_t ra) noexcept;
void recordDispatcherSelection268910(const R5900Context *ctx, uint32_t sp,
                                     uint32_t ra) noexcept;
// arrivalSource distinguishes the opening switch (268910), initial fall-through
// (26890c), and local back-edge (268b38), even when preSwitchPc stays unchanged.
void recordOwnerLabel268910(const R5900Context *ctx, uint32_t preSwitchPc,
                            uint32_t sp, uint32_t ra, uint32_t arrivalSource,
                            const uint8_t *rdram = nullptr) noexcept;
void clearOwnerActiveFrame(const R5900Context *ctx, uint32_t postPrologueSp) noexcept;

} // namespace rrv::m2pgame001

#endif // RRV_M2P_GAME001_FAIL_CLOSED_H
