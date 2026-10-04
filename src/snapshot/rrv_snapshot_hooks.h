// rrv_snapshot_hooks.h — the ENTIRE surface the vendored PS2 runtime
// (tools/PS2Recomp/ps2xRuntime) is allowed to see of the developer snapshot
// system (src/snapshot/).  Same contract as src/ir/rrv_ir_hooks.h and
// src/gs-record/rrv_gs_record_hooks.h:
//
//   * every hook is a no-op unless the feature was armed from the command line,
//   * the enabled() check is a single relaxed atomic load, and the vendored call
//     site guards the call so even argument marshalling is skipped when off,
//   * no outer type ever crosses this boundary.
//
// Three call sites only (tools/patches/ps2recomp-runtime-snapshot-hooks.patch):
//
//   1. PS2Runtime::run(), immediately before the game thread is spawned
//        -> hookRunStart()      — applies a pending --snapshot restore.
//   2. PS2Runtime::dispatchLoop(), top of the dispatch loop
//        -> hookDispatchBoundary() — the SAFEPOINT. This is the only place in
//          the runtime where a guest thread holds no recompiled-function host
//          frames, so it is the only place a resumable capture may be taken.
//          See docs/SNAPSHOTS.md §3.
//   3. Kernel/Syscalls/Thread.cpp guest-thread worker, entry and exit
//        -> hookGuestThreadStart() / hookGuestThreadExit() — publishes each
//          guest thread's live R5900Context so a capture can see it. The
//          context otherwise lives only on that worker's host stack.
//
// Everything else (format, file IO, CLI, registry) stays outside the vendored
// tree and is committable.

#ifndef RRV_SNAPSHOT_HOOKS_H
#define RRV_SNAPSHOT_HOOKS_H

#include <cstdint>

struct R5900Context;
class PS2Runtime;

namespace rrv::snapshot {

// True when either a save or a restore was requested. Off => every hook below
// returns immediately.
bool enabled();

// A restore is pending (--snapshot <name>). Applied by hookRunStart().
bool restorePending();

// PS2Runtime::run(): the guest image is loaded, the HLE kernel has been reset
// and the default entry context is set, but no guest thread runs yet. This is
// where a restore overwrites guest memory, devices and CPU state.
void hookRunStart(PS2Runtime *runtime);

// PS2Runtime::dispatchLoop(): called with `ctx` at a resumable PC and no guest
// host frames below it. Services a pending capture request when its trigger has
// fired; otherwise returns after a counter bump.
void hookDispatchBoundary(PS2Runtime *runtime, R5900Context *ctx);

// Kernel guest-thread worker lifecycle. `ctx` is the worker's own context; the
// snapshot module only ever reads it, and only from a thread that is itself
// parked at a dispatch safepoint.
void hookGuestThreadStart(int32_t tid, R5900Context *ctx);
void hookGuestThreadExit(int32_t tid);

// A GIF packet has been handed to the arbiter (GifArbiter::submit — the same
// boundary the GS recorder observes). Only the shape is passed, never the data:
// this exists so a checkpoint can be triggered by a *display-list signature*
// ("the first PATH3 chain of at least N bytes"), which for scenes that have no
// convenient state variable is the most reliable deterministic marker
// available. Arms the latch; the capture still happens at the next safepoint.
void hookGifPacket(uint8_t pathId, uint32_t sizeBytes);

} // namespace rrv::snapshot

#endif // RRV_SNAPSHOT_HOOKS_H
