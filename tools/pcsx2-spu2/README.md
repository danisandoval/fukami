# rrv_pcsx2_spu2

PCSX2 2.8.2's SPU2 core, compiled **unmodified** from the pinned checkout,
behind a small timestamped C ABI (`rrv_pcsx2_spu2.h`), with an optional worker
thread. It is meant to be driven by a host-side HLE of the game's IOP sound
driver (not part of this directory). GPL-3.0+; see `PROVENANCE.md`.

## Build and test

```sh
cmake -S tools/pcsx2-spu2 -B ../build-pcsx2-spu2-dev -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DRRV_PCSX2_SOURCE_DIR="$REPO/build-deps/pcsx2-2.8.2"
cmake --build ../build-pcsx2-spu2-dev
ctest --test-dir ../build-pcsx2-spu2-dev --output-on-failure
../build-pcsx2-spu2-dev/spu2_selftest bench 10   # CPU cost, inline vs worker
```

Targets: `rrv_pcsx2_spu2` (static lib), `spu2_selftest`, and (Clang/GCC,
`RRV_SPU2_TSAN_TESTS=ON`) a ThreadSanitizer build `rrv_pcsx2_spu2_tsan` +
`spu2_selftest_tsan`. ctest runs `spu2_tone`, `spu2_determinism`,
`spu2_threading`, `spu2_threading_tsan`. `RRV_PCSX2_SOURCE_DIR` defaults to
`<repo>/build-deps/pcsx2-2.8.2`; pass it explicitly from a worktree.

## ABI summary

| Call | Mode | Meaning |
|---|---|---|
| `rrv_spu2_create/destroy` | — | one instance per process at a time (PCSX2 globals); `create` returns NULL if one is live. Sequential create/destroy is fine and gives a clean power-on state each time. |
| `write16(t, addr, v)` | async | register write (`0x1F900000`-based; offsets `<0x10000` are rebased) |
| `read16(t, addr)` | barrier | register read, same value as inline |
| `dma_write(t, core, addr, data, n)` | async | payload copied at call time; **models a core DMA** (see below) |
| `dma_read(t, core, addr, out, n)` | barrier | SPU RAM snapshot at `t` + a real core DMA read for side effects |
| `advance(t)` | async | mix up to `t` |
| `sync(t, out, max)` | barrier | process through `t`; return up to `max` events (≤ `t`, in order); the rest stay queued |
| `pull_output(buf, frames)` | any 1 thread | SPSC ring → float stereo 48 kHz; silence on underrun |
| `get_stats` | any thread | relaxed counters |
| `state_hash(t)` | barrier | hash of guest-visible state |

Events: `1` SPU IRQ (`data` = mask of cores that newly raised it), `4` DMA4
(core 0) done, `7` DMA7 (core 1) done. `iop_cycle` is the exact guest cycle:
IRQs raised while mixing a frame carry that frame's end cycle (`lClocks`),
DMA completions carry their countdown deadline.

### DMA model

`dma_write` = what the guest's `sceSdVoiceTrans` does: write TSA (hi, lo) as
register writes unless `spu_addr16 == RRV_SPU2_KEEP_TSA`, then run PCSX2's
`SPU2writeDMA4Mem/7Mem` from a private IOP-RAM staging area (1 MiB per core,
so `n ≤ 0x80000`; longer requests are truncated and counted). PCSX2 copies
the first `0x100 + n/24` halfwords at once and the rest at each countdown
(24 IOP cycles/halfword), sets/clears STATX bit 10, checks IRQA against the
written range, and raises the DMA-done event at its deadline. The HLE should
still write ATTR DMA-mode bits itself if the driver does. Starting a new DMA on
a core whose previous one is still busy is counted in `dma_overlaps`
(it reuses the staging area, as the hardware would misbehave too).

`dma_read` returns SPU RAM `[TSA, TSA+n)` as of `t` immediately, and starts
PCSX2's DMA read so TSA/STATX/IRQA side effects and the done event match
PCSX2 timing. PCSX2's own read copies after the delay; the two differ only if
the mixer writes that range meanwhile (the `0x0000-0x27FF` dynamic area).

## Clock, threading and determinism model

- The only clock is `psxRegs.cycle`, set from each command's `t`. The core
  mixes one frame per 768 IOP cycles (`TimeUpdate`). `run_to(t)` advances in
  steps that stop at every pending DMA countdown deadline (what PCSX2's IOP
  counter 6 schedules) and never exceeds 768·1024 cycles, so TimeUpdate's
  sanity clamp (768·4800) never skips audio.
- Timestamps must be non-decreasing; a smaller one is clamped to the last and
  counted (`clamp_violations`). This is done on the calling thread, so it is
  identical in both modes.
- Threaded mode: commands go into a mutex+condvar queue in call order and a
  single worker executes them; barriers block the caller until the worker has
  executed them. The core executes the same command sequence in both modes, so
  registers, events (with cycles), state hashes and mixed samples are
  byte-identical (tested). Host time is used only for the optional
  `worker_cpu_ns` stat.
- All calls except `pull_output`/`get_stats` must come from one thread.
- Output ring: lock-free SPSC. The producer never waits: a full ring drops the
  new frame (`overrun_frames`); an empty ring yields silence
  (`underrun_frames`). `output_hash` hashes every mixed s16 frame
  independently of the ring. The float stage applies upstream's DC-blocking
  filter.

## Supported / not supported

Exercised by the selftest: plain DMA writes on both cores (single-shot and
multi-countdown), DMA read, voice key-on/off, ADSR attack/sustain, fixed
volumes, looping ADPCM, IRQA hits from a playing voice + re-arm via ATTR,
core 0 -> core 1 external mix, reverb enabled on core 1, register reads.
Compiled in (upstream code, not specifically tested here): volume slides,
noise, pitch modulation, 0x1AC register-port writes, the rest of the reverb
parameter space. `spu2freeze.cpp` is compiled but not exposed.

Not supported / not tested: **AutoDMA (ADMA) streaming** (if the guest enables
ADMA on a core, `dma_write` goes down PCSX2's ADMA path whose data pointer is
our staging area — untested), PS1 mode, S/PDIF bypass/CDDA, save states via
the ABI, PCSX2_DEVBUILD logging. More than one instance at once.
