// SPDX-License-Identifier: GPL-3.0+
// Gate 8: host re-implementation of RR5's IOP sound driver RSPU2DRV.IRX
// ("rspu2 driver 1.2.0", libspu2/libsnd2 over SIF RPC SID 0x80000601),
// driving the PCSX2 SPU2 core in tools/pcsx2-spu2 (inline mode, owned by the
// driver). Behaviour follows local/gate8/rspu2-driver-spec.md; this is our own
// code written from that behavioural description (no driver code is copied).
//
// Time: every call carries a guest IOP cycle count (36.864 MHz) derived by the
// caller from guest time only. Every guest-visible result (reply bytes,
// callbacks and their cycles, IOP-memory writes, SPU2 state, mixed samples) is
// a pure function of the ordered call sequence; host timing and thread
// scheduling never reach it. Timestamps must be non-decreasing; a smaller one
// is clamped to the last one seen (counted in Stats::clamp_violations).
//
// Threading: in threaded mode one driver thread executes an ordered queue.
// rpc(), sync() and the debug*() calls are barriers; everything else only
// enqueues. A barrier on an idle driver (empty queue) runs on the caller.
// In inline mode everything runs on the caller. Both modes run the same code
// on the same op sequence and give identical results. All calls except
// pullOutput()/stats() must come from one thread.
//
// Addresses: every "IOP address" in this API (iopWrite, IopWrite records, RPC
// arguments such as VAB header/body pointers, SpuStEnv data_addr, the 0x0200
// reply) is in the game's IOP-window namespace [iop_window_base,
// iop_window_base + iop_window_size). The driver's IOP-visible data lives at
// driver_block: the SpuStEnv mirror (0x188 bytes) is at driver_block +
// kMirrorOffset; reserve kDriverBlockSize bytes there.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rrv::rspu2
{
constexpr uint32_t kRpcSid = 0x80000601u;
constexpr uint64_t kIopHz = 36864000;          // IOP cycles per second
constexpr uint64_t kCyclesPerSample = 768;      // one 48 kHz output frame
constexpr uint32_t kMirrorOffset = 0x0;         // SpuStEnv mirror inside driver_block
constexpr uint32_t kEnvSize = 0x188;            // SpuStEnv size
constexpr uint32_t kDriverBlockSize = 0x200;    // bytes the driver uses at driver_block

struct Config
{
    uint32_t iop_window_base = 0x01A00000u; // guest address of the IOP-memory window
    uint32_t iop_window_size = 0x00500000u;
    uint32_t driver_block = 0x01A00000u + 0x00500000u - 0x1000u; // inside the window
    bool threaded = true;                   // false: everything inline on the caller
    uint32_t output_ring_frames = 0;        // SPU2 output ring (0 = library default)
};

// IOP->EE mailbox event (spec 12): 1 prepare-finished (a1 bits, a2 4|6),
// 2 transfer-finished (bits, 6), 3 stream-finished (bits, 6|8), 4 autoDMA,
// 5 transfer callback (0x8100), 6 IRQ callback (0x8200). Types 4-6 carry the
// mailbox's previous a1/a2 (the real stubs do not write them).
struct Callback
{
    uint64_t t;
    uint32_t type, a1, a2;
};

struct AsyncReply
{
    uint64_t id;
    uint64_t t; // IOP cycle at which the driver finished the command
    std::vector<uint8_t> bytes;
};

// The driver wrote IOP memory (VAB header patches, ADPCM flag patches, stream
// end-block save/restore). Already applied to the driver's own shadow; the
// caller mirrors it into guest memory at a deterministic point.
struct IopWrite
{
    uint64_t t;
    uint32_t addr;
    std::vector<uint8_t> bytes;
};

class Driver
{
public:
    explicit Driver(const Config&);
    ~Driver();
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    // False if the SPU2 core could not be created (one instance per process).
    bool ok() const;

    // The guest wrote IOP memory (SIF DMA, CD read to IOP). Copied; ordered.
    void iopWrite(uint64_t t, uint32_t addr, const void* src, uint32_t n);

    // Blocking RPC to SID 0x80000601 (fno = command). Barrier. Writes
    // reply_size bytes of the buffer the real driver returns. Returns the IOP
    // cycle at which the command finished (> t when the real driver
    // busy-waits: SpuInit, the batch's 30 us delays, TransCompleted(1), the
    // reverb work-area clear).
    uint64_t rpc(uint64_t t, uint32_t fno, const void* recvbuf, uint32_t size, void* reply,
                 uint32_t reply_size);

    // No-wait RPC; the reply is returned by sync() once its finish cycle <= t.
    uint64_t rpcAsync(uint64_t t, uint32_t fno, const void* recvbuf, uint32_t size,
                      uint32_t reply_size);

    // The EE handler for a delivered callback has returned (the real
    // callback thread's sceSifCallRpc came back). For types 1..3 the driver
    // then copies the SpuStEnv mirror to its internal env at t.
    void callbackDelivered(uint64_t t, uint32_t type);

    // Guest time passes. Non-blocking in threaded mode.
    void advance(uint64_t t);

    // Barrier through t. Appends (in order) and removes everything with
    // timestamp <= t.
    void sync(uint64_t t, std::vector<AsyncReply>& replies, std::vector<Callback>& callbacks,
              std::vector<IopWrite>& writes);

    // Barrier that collects only what has timestamp <= upTo, where upTo may
    // be earlier than times already submitted (it does not move the
    // timeline). Used to deliver callbacks between VBlanks one quantum
    // behind the guest, so the driver thread has usually already got there.
    void syncUpTo(uint64_t upTo, std::vector<AsyncReply>& replies, std::vector<Callback>& callbacks,
                  std::vector<IopWrite>& writes);

    // Any single consumer thread (SDL callback). Interleaved float stereo at
    // 48 kHz; silence on underrun. Returns frames of real audio delivered.
    size_t pullOutput(float* stereo, size_t frames);

    struct Stats
    {
        uint64_t frames_mixed = 0;
        uint64_t underrun_frames = 0;
        uint64_t overrun_frames = 0;
        uint64_t rpc_count = 0;          // blocking + async
        uint64_t batch_entries = 0;      // commands executed inside 0xFFFE batches
        uint64_t barrier_waits = 0;      // caller-side barrier waits (threaded mode, worker busy)
        uint64_t barrier_wait_ns = 0;    // host time the caller spent blocked in them
        uint64_t queue_high_water = 0;   // max ops pending for the driver thread
        uint64_t clamp_violations = 0;   // caller timestamps that went backwards
        uint64_t unknown_commands = 0;   // fnos / batch words this driver does not implement
        uint32_t last_unknown_fno = 0;
        uint64_t iop_out_of_window = 0;  // IOP accesses outside the window (read 0 / dropped)
        uint64_t dma_transfers = 0;      // ch7 transfers started
        uint64_t callbacks = 0;          // mailbox events emitted
        uint64_t callbacks_dropped = 0;  // posted before 0xE621 created the callback thread
        uint64_t hangs_avoided = 0;      // busy-waits the real driver would never leave
        uint64_t spin_timeouts = 0;      // register polls that hit their iteration cap
        uint64_t spu2_clamp_violations = 0;
        uint64_t spu2_dma_overlaps = 0;
        uint64_t driver_cycle = 0;       // driver timeline position
        uint64_t stale_refills = 0;      // stream IRQs that found a refill callback still undelivered
        uint64_t slow_ops = 0;           // driver ops that took > 1 ms of host time
        uint64_t max_op_ns = 0;          // slowest driver op (host ns)
    };
    Stats stats() const;

    // Debug / test accessors (barriers).
    uint64_t debugSpu2StateHash(uint64_t t);
    uint16_t debugReadSpu2(uint64_t t, uint32_t reg_offset); // offset from 0x1F900000
    uint64_t debugOutputHash();                              // valid after a barrier

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Process-wide instance for the candidate: creates the driver (threaded) and,
// in the RRV_GATE8_SPU2 build, feeds SDL from its output ring. Idempotent.
Driver* start(const Config& config = Config{});
Driver* instance();
void stop();
} // namespace rrv::rspu2
