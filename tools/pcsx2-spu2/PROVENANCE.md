# Provenance — rrv_pcsx2_spu2

Upstream: PCSX2 2.8.2, commit `fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3`
(`config/dependencies.lock.toml` `[pcsx2]`), checkout `build-deps/pcsx2-2.8.2`.
Licence: GPL-3.0+ (SPDX headers kept on every file; this directory is GPL-3.0+).

## Compiled unmodified from the pinned checkout (not copied into the repo)

| File (pcsx2/SPU2/) | sha256 prefix |
|---|---|
| ADSR.cpp | faea787bf4f132ae |
| Dma.cpp | 322cd94a54e292a8 |
| Mixer.cpp | 94a43b48c65cd724 |
| ReadInput.cpp | 72adb34a7f789df2 |
| RegTable.cpp | 0fcfd9a2ca7be910 |
| Reverb.cpp | 8dbb9d294ea58660 |
| ReverbResample.cpp | a10c04edc1f590fe |
| spu2sys.cpp | f6b3a5d337e9df0e |
| spu2freeze.cpp | 69e194af5aae90bd |
| defs.h, regs.h, spu2.h, Debug.h, Dma.h, interpolate_table.h | c99bb3de…, 6ca0eac6…, 8669becf…, de7d6c30…, 275343e4…, 3c2f96bd… |

**Patched PCSX2 files: none.** There is no `patched/` directory.

Not compiled: `spu2.cpp` (AudioStream/VMManager/GSCapture glue, replaced — see
below), `Debug.cpp`, `Wavedump_wav.cpp` (PCSX2_DEVBUILD only).

## Adapted code (in `rrv_pcsx2_spu2.cpp`)

From `pcsx2/SPU2/spu2.cpp` @ fd9d310c (sha256 prefix bd288bb73ccaa0b3):
`StereoOut32::Empty`, `lClocks`, `SPU2read()`, `SPU2write()`,
`SPU2{read,write}DMA{4,7}Mem()`, `SPU2interruptDMA{4,7}()`, `SPU2async()`,
`SPU2::IsRunningPSXMode()` (always false), `SPU2::InternalReset(false)` +
`SPU2::Open()` (as `core_power_on`), `DCFilter()` + `spu2Output()`.
Changes: logging removed; `AudioStream::WriteChunk` replaced by an SPSC ring;
`spu2Mix` points at a wrapper around `isa_native::spu2Mix` that marks IRQs raised
inside a mix tick; the power-on path zeroes all core globals and flushes
`spu2sys.cpp`'s `static has_to_call_irq_dma[2]` (a one-cycle DMA countdown with
IRQs disabled) so a second instance starts identical to the first.

From `pcsx2/IopDma.cpp` `spu2DMA4Irq()`/`spu2DMA7Irq()` and `pcsx2/IopIrq.cpp`
`spu2Irq()`: same call into `SPU2interruptDMA*` and CHCR busy-bit handling; the
IOP interrupt is replaced by a timestamped `rrv_spu2_event`.

From `pcsx2/IopDma.cpp` `psxDmaGeneric()`: CHCR values `0x01000201`
(IOP→SPU2) / `0x01000200` (SPU2→IOP).

## Shims (`shim/`, written for RRV, mirror upstream declarations)

`common/Pcsx2Types.h`, `common/Pcsx2Defs.h` (release: `IsDevBuild=false`),
`common/Assertions.h` (no-op), `common/Console.h` (no-op), `GS/MultiISA.h`
(upstream's non-multi-ISA branch), `GS/GSVector.h` (scalar `GSVector4i` with
pmulhrsw/paddsw/phaddsw lane semantics: bit-exact with upstream's `_sse`
reverb resampler; upstream AVX2 builds use `_avx`, which can differ only when a
16-bit partial sum saturates), `Host/AudioStream.h` (empty), `Config.h`,
`SaveState.h`, `R3000A.h` (`psxRegs.cycle` only), `IopCounters.h` (sinks),
`IopHw.h` (DMA4/7 regs over a private array), `IopMem.h` (`iopPhysMem` over a
private 2 MiB buffer), `IopDma.h`, `rrv_spu2_prelude.h` (stands in for PCSX2's
precompiled header).

Build flags: `-fno-strict-aliasing` as upstream; `-ffp-contract=off` instead of
upstream's `fast` (it only touches the host-side float output stage).
