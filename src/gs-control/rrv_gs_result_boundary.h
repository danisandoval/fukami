// SPDX-FileCopyrightText: 2026 RRV-Recomp contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Guest-result admission is deliberately a producer-side decision.  A PCSX2
// local-memory call is synchronous only after all guest-owned work relevant to
// it is complete; it must never consume a retained suffix, acknowledge SIGNAL,
// or turn a worker drain into an implicit guest action.
#pragma once

#include <cstdint>

namespace rrv::gs {

enum class ResultOperation : uint8_t {
    ReadLocalMemory,
    SnapshotLocalMemory,
    RestoreLocalMemory,
};

enum class ProducerWork : uint32_t {
    None                 = 0u,
    // GIF work admitted to the producer stream but not yet guest-complete.
    // Synchronously submitted work which has returned from the bridge does not
    // remain represented here.
    RendererVisibleGif   = 1u << 0,
    DeferredVif1         = 1u << 1,
    PartialDirect        = 1u << 2,
    BlockedGifDma        = 1u << 3,
    VifCompletion        = 1u << 4,
    // A replay is not a second renderer queue.  It is still non-quiescent: a
    // result operation cannot observe and mutate state mid-replay.
    ReplayOnly           = 1u << 5,
    // Default RRV GIF-path latency retains complete VIF1 DMA chains until a
    // guest frame/ordering flush.  They have not reached VIF/GIF yet.
    HeldVif1             = 1u << 6,
    // A DIRECT IMAGE tag can require raw PATH2 qwords from a later VIF1 DMA
    // transfer.  The partial IMAGE transaction is producer-owned work.
    PendingPath2Image    = 1u << 7,
    // MSKPATH3 can retain complete PATH3 packets outside the GIF arbiter until
    // a later VIF1 unmask.  They remain guest-owned and must not be skipped by
    // a local-memory result operation.
    MaskedPath3           = 1u << 8,
};

constexpr ProducerWork operator|(ProducerWork left, ProducerWork right) {
    return static_cast<ProducerWork>(static_cast<uint32_t>(left) |
                                     static_cast<uint32_t>(right));
}
constexpr ProducerWork &operator|=(ProducerWork &left, ProducerWork right) {
    left = left | right;
    return left;
}
constexpr bool any(ProducerWork work) {
    return static_cast<uint32_t>(work) != 0u;
}

struct ResultBoundary {
    ProducerWork work = ProducerWork::None;

    // A full local-memory image is useful GS state, but never by itself a
    // complete runtime checkpoint: CPU/device/DMA/GIF/VIF/IRQ state remains
    // producer-owned and must be separately captured by a future checkpoint
    // contract.
    bool admits(ResultOperation) const { return !any(work); }
};

inline const char *resultOperationName(ResultOperation operation) {
    switch (operation) {
    case ResultOperation::ReadLocalMemory: return "local-memory read";
    case ResultOperation::SnapshotLocalMemory: return "local-memory snapshot";
    case ResultOperation::RestoreLocalMemory: return "local-memory restore";
    }
    return "local-memory operation";
}

} // namespace rrv::gs
