# Third-party notices

Fukami is licensed under the GNU GPL v3.0 ([LICENSE](LICENSE)). It includes, links or is derived from the following.
Each keeps its own licence and copyright notices, in its source files and in the licence files of the downloaded
sources.

| Component | Where | Licence |
|---|---|---|
| [PCSX2](https://github.com/PCSX2/pcsx2) 2.8.2 (GS renderer, SPU2 core, bundled libchdr/lzma, Dear ImGui and others) | fetched at the pinned revision (`config/dependencies.lock.toml`) and patched with `tools/patches/pcsx2/`; derived code in `tools/pcsx2-gs-bridge`, `tools/pcsx2-spu2` | GPL-3.0-or-later, plus its components' licences |
| [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j: recompiler, analyzer, runtime | `third_party/ps2recomp` (see `RRV_CHANGES.md` for our changes; upstream's own licence file is `third_party/ps2recomp/LICENSE`) | GPL-3.0 |
| PS2Recomp's SCE symbol database | `third_party/ps2recomp/ps2xAnalyzer/include/ps2recomp/sce_symbol_database_data.h`, as in upstream | as upstream |
| [SDL](https://libsdl.org/) 2.32.10 | fetched and built by `scripts/prepare_sdl.py` | zlib |
| [libchdr](https://github.com/rtissera/libchdr), zstd, LZMA SDK | via PCSX2's sources | BSD-3-Clause, BSD, public domain |
| [sse2neon](https://github.com/DLTcollab/sse2neon) | fetched at a pinned revision | MIT |
| Libraries bundled in release builds (freetype, libjpeg, libpng, libwebp, lz4, ryml, zstd, SDL3 and the Linux set) | listed in the `LICENSE-NOTICES.txt` inside each release | their own licences |

Fukami's own icon (`tools/fukami-app/fukami.png`) is original artwork released under the same GPL-3.0 licence.
