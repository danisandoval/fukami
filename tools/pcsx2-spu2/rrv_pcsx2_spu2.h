// SPDX-License-Identifier: GPL-3.0+
// rrv_pcsx2_spu2 — PCSX2 2.8.2 SPU2 core behind a small, timestamped C ABI.
//
// Clock model: every call carries a guest IOP cycle count `t` (IOP clock,
// 36.864 MHz; one 48 kHz output frame = 768 cycles). Host time never reaches
// the core. Timestamps must be non-decreasing across calls; a smaller value
// is clamped to the last one seen and counted in stats.clamp_violations.
//
// Threading: all functions except rrv_spu2_pull_output() and rrv_spu2_get_stats()
// must be called from one thread (or be externally serialised). In threaded
// mode those calls enqueue commands for a worker thread; calls documented as
// "barrier" block until the worker has processed everything up to them and
// return exactly what inline mode returns.
//
// One instance per process at a time (the PCSX2 core uses globals);
// rrv_spu2_create() returns NULL while another instance exists.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rrv_spu2 rrv_spu2;

typedef struct rrv_spu2_config
{
	int threaded;                /* 0 = inline (core runs on the caller), 1 = worker thread */
	uint32_t output_ring_frames; /* stereo frames in the output ring; rounded up to a power of two; 0 = 8192 */
} rrv_spu2_config;

enum
{
	RRV_SPU2_EVENT_SPU_IRQ = 1,   /* data = core mask of newly raised IRQs (bit0 core0, bit1 core1) */
	RRV_SPU2_EVENT_DMA4_DONE = 4, /* core 0 DMA transfer complete (IOP DMA ch.4 interrupt) */
	RRV_SPU2_EVENT_DMA7_DONE = 7, /* core 1 DMA transfer complete (IOP DMA ch.7 interrupt) */
};

typedef struct rrv_spu2_event
{
	uint64_t iop_cycle; /* exact guest IOP cycle at which the core raised it */
	uint32_t kind;      /* RRV_SPU2_EVENT_* */
	uint32_t data;
} rrv_spu2_event;

typedef struct rrv_spu2_stats
{
	uint64_t frames_mixed;       /* 48 kHz frames produced by the core */
	uint64_t underrun_frames;    /* silence frames handed out by pull_output */
	uint64_t overrun_frames;     /* frames dropped because the ring was full */
	uint64_t clamp_violations;   /* calls whose timestamp went backwards (clamped) */
	uint64_t queue_high_water;   /* max commands pending in the worker queue */
	uint64_t commands;           /* commands executed by the core */
	uint64_t dma_overlaps;       /* DMA started while the same core's DMA was still busy */
	uint64_t dma_truncated;      /* DMA requests longer than 0x80000 halfwords (truncated) */
	uint64_t output_hash;        /* running hash of every mixed s16 frame (valid after a barrier) */
	uint64_t worker_cpu_ns;      /* worker thread CPU time (threaded mode only; host-side stat) */
} rrv_spu2_stats;

/* Pass as spu_addr16 to keep the core's current TSA instead of writing it. */
#define RRV_SPU2_KEEP_TSA 0xFFFFFFFFu

rrv_spu2* rrv_spu2_create(const rrv_spu2_config* cfg);
void rrv_spu2_destroy(rrv_spu2* s);

/* SPU2 register write. reg_addr is a full 0x1F900000-based address (an offset
 * below 0x10000 is accepted and rebased). Async in threaded mode. */
void rrv_spu2_write16(rrv_spu2* s, uint64_t t, uint32_t reg_addr, uint16_t v);

/* SPU2 register read. Barrier. */
uint16_t rrv_spu2_read16(rrv_spu2* s, uint64_t t, uint32_t reg_addr);

/* Core DMA write (IOP -> SPU RAM), through PCSX2's normal DMA4/DMA7 path:
 * if spu_addr16 != RRV_SPU2_KEEP_TSA the core's TSA register is written first
 * (hi then lo, exactly like a guest register write), then the payload
 * (copied at call time) is transferred with PCSX2's DMA timing, STATX bits and
 * IRQA checks; completion raises RRV_SPU2_EVENT_DMA4/7_DONE at its guest cycle.
 * AutoDMA (ADMA) is not supported: if the guest enabled ADMA on the core the
 * transfer goes down PCSX2's ADMA path and is unsupported. Async in threaded
 * mode. count16 is capped at 0x80000 (1 MiB). */
void rrv_spu2_dma_write(rrv_spu2* s, uint64_t t, int core, uint32_t spu_addr16, const uint16_t* data, uint32_t count16);

/* Core DMA read (SPU RAM -> IOP). Barrier. Writes TSA like dma_write, copies
 * SPU RAM [TSA, TSA+count16) as of cycle t into `out`, and starts PCSX2's
 * DMA read so TSA/STATX/IRQA side effects and the completion event happen
 * with PCSX2 timing. (The data PCSX2 would copy after the transfer delay can
 * differ from `out` only if the mixer writes that range in the meantime,
 * i.e. only inside the 0x0000-0x27FF dynamic area.) */
void rrv_spu2_dma_read(rrv_spu2* s, uint64_t t, int core, uint32_t spu_addr16, uint16_t* out, uint32_t count16);

/* Mix up to t. Async in threaded mode. */
void rrv_spu2_advance(rrv_spu2* s, uint64_t t);

/* Barrier: process everything through t, then return up to `max` pending
 * events (all with iop_cycle <= t) in order. Events not returned stay queued. */
size_t rrv_spu2_sync(rrv_spu2* s, uint64_t t, rrv_spu2_event* out, size_t max);

/* Any single consumer thread (e.g. the SDL audio callback). Copies `frames`
 * interleaved float stereo frames out of the SPSC ring; silence on underrun.
 * Never blocks and never back-pressures the core. Returns frames of real
 * audio delivered (the rest is silence). */
size_t rrv_spu2_pull_output(rrv_spu2* s, float* interleaved_stereo, size_t frames);

/* Any thread. Counters are relaxed snapshots. */
void rrv_spu2_get_stats(rrv_spu2* s, rrv_spu2_stats* out);

/* Barrier: hash of guest-visible SPU2 state at t (register file, SPU RAM,
 * per-core/per-voice state incl. ADSR/volume slides/decoder, DMA state,
 * mixer position, core clock). For determinism tests. */
uint64_t rrv_spu2_state_hash(rrv_spu2* s, uint64_t t);

#ifdef __cplusplus
}
#endif
