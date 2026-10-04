# Architecture

How Fukami is put together and what has to stay true when you change it.

## The pipeline

```text
  your disc (CHD) ──first launch──▶ unpacked game files + SLUS_200.02 (your ELF, checked by SHA-256)
                                                  │  scripts/fukami_generate.py (build from source)
                                                  ▼
  generated/rr5/output  (the game's code as C++, generated from your ELF, ignored by git, compiled in place)
        │
  override layer  src/product/patches.cpp  (function replacements, native hot functions, enhancements)
        │
        ▼
  runtime  third_party/ps2recomp/ps2xRuntime  +  src/guest-time  +  src/vu-aot  +  src/hle
     guest-time owner (VBlank/FIELD, EE timers, INTC, RTC) · DMA/VIF/GIF · syscalls · memory card
     VU0/VU1 microcode recompiled ahead of time
     VIF1/VU1/GS stream: inline on the EE thread, or on one owner thread (owner-async, the default)
        │  versioned RRV ABI 5, no PCSX2 headers across it         src/gs-backend
        ▼
  PCSX2 2.8.2 GS (the only renderer; bridge built from the pinned checkout plus tools/patches/pcsx2/)
        │  Metal (macOS) / Vulkan (Linux), direct GPU presentation
        ▼
  SDL window, input, audio output   ◀── audio: the game's IOP sound driver over the PCSX2 SPU2 core
  Fukami app (src/app): disc extraction, settings, in-game menu, launcher
```

## Components

| Part | Where | Role |
|---|---|---|
| Game code | `generated/rr5/` (local, ignored) | C++ generated from your ELF by the recompiler, plus the ahead-of-time VU blocks and the hot-function file. Never edited; `config/rr5/` holds the hashes it must match. |
| Override layer | `src/product/` | Host entry (`main_product.cpp`, ELF identity check) and `patches.cpp`: the only place game behaviour changes. |
| Runtime | `third_party/ps2recomp/ps2xRuntime` | Ordinary C++ edited in place (`RRV_CHANGES.md` lists changes against the upstream import). Owns memory, syscalls, DMA/VIF/GIF, memory card. |
| Guest time | `src/guest-time` | One producer-owned virtual clock. VBlank/FIELD, EE timers, INTC and the RTC follow modelled guest cycles; the host only paces. |
| VU microcode | `src/vu-aot`, `tools/vu-aot` | VU0/VU1 programs compiled to C++ ahead of time, with the interpreter as the semantic oracle and fallback. No run-time code generation. |
| Hot functions | `tools/ee-native` | Exact copies of the hottest generated functions with cheaper cycle accounting. |
| GS | `src/gs-backend`, `tools/pcsx2-gs-bridge`, `tools/patches/pcsx2/` | GS work goes through ABI 5 to PCSX2's GS. |
| Audio | `src/audio`, `tools/pcsx2-spu2` | The IOP sound driver on a driver thread against the PCSX2 SPU2 core; SDL only outputs. |
| Settings | `src/app/fukami_settings.cpp` | The one schema: `rrv.ini`, the launcher and the in-game menu all use it. |
| App | `src/app` | First-launch CHD extraction, settings, menu. |
| Build | `cmake/`, `scripts/` | Compiles the directories above in place. See [BUILDING.md](../BUILDING.md). |

## Invariants

1. **One GS.** PCSX2's GS is the only renderer.
2. **Guest time belongs to the producer.** Nothing host-side changes what the guest observes; it must see exact FIELD service.
   Progressive output is a presentation choice.
3. **No build step edits or copies source.** The product compiles directories in place.
4. **Generated code is read-only.** A change inside a function needs a recompiler hook point and a regeneration.
5. **User data stays the user's.** No disc image, ELF/IRX, BIOS, asset or capture is committed or shipped; the product refuses
   an ELF other than the one whose hash is in `config/rr5/source-manifest.json`.
6. **Behaviour baseline.** A change that claims "no behaviour change" must reproduce the traced runs and per-start digests
   byte for byte (see [TESTING.md](TESTING.md)).
