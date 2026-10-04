# Fukami

**Fukami** is a native port of **Ridge Racer V** (PlayStation 2, USA, `SLUS-20002`) for macOS (Apple silicon) and
Linux (Steam Deck) (Windows coming soon), made by static recompilation: the game's MIPS code is translated ahead of time into C++ and
compiled into a normal application, with a PS2 hardware layer (the GS from [PCSX2](https://pcsx2.net/)) underneath.
It is **not an emulator** and it cannot run any other game.

> **This repository and its releases contain no game data.** You need your own copy of Ridge Racer V (USA),
> dumped from a disc you own. Fukami is not affiliated with, authorised or endorsed by Bandai Namco or Sony.
> See [LEGAL.md](LEGAL.md).

## Features

- Native 60 fps progressive output; the game's logic still sees the exact PS2 timing.
- Internal resolution up to 8x, PCSX2 anti-aliasing emulation, FXAA, CAS sharpening, anisotropic filtering, mipmaps.
- **Real widescreen** (16:9, 16:10, 21:9): the game's camera renders a wider view, not a stretched picture. The
  intro keeps its own letterboxed framing.
- Optional enhancements: more cars at full detail (car LOD), longer draw distance, faster loading.
- Controller support (SDL game controllers, rumble) and keyboard; in-game settings menu (Esc / F1 / Guide button).
- Saves to a real memory-card file, safe against power loss.
- No JIT anywhere: the VU microcode and hot functions are compiled ahead of time, so it runs on platforms that
  forbid run-time code generation.

## Requirements

| | |
|---|---|
| **macOS** | Apple silicon (M1 or later); the minimum macOS version is in each release's notes. |
| **Steam Deck / SteamOS** | SteamOS 3.7 or later. Other Linux x86-64 distributions are untested. |
| **Game** | Ridge Racer V (USA) as a **CHD** file made from your own disc. About 500 MB of free disk space. |

Windows is planned; see the [roadmap](docs/ROADMAP.md).

## Install

Download the latest build from the [Releases](https://github.com/danisandoval/fukami/releases) page (and only from there):

- **macOS:** unzip `Fukami-<version>-macos-arm64.zip`, move **Fukami** to Applications, then right-click it and choose
  **Open** the first time (the app is not notarised yet).
- **Steam Deck:** in Desktop Mode, extract `Fukami-<version>-linux-x86_64.tar.gz` and run `./install.sh`; it adds
  Fukami to Steam. [Step-by-step guide](docs/USER_GUIDE.md#steam-deck).

On first launch Fukami asks for your CHD, checks that it is the USA disc, and unpacks the game files once into its
own folder. Full instructions, controls and settings are in the [user guide](docs/USER_GUIDE.md).

### Getting your CHD

Dump your own Ridge Racer V (USA) disc to an image with any standard tool, then convert it with `chdman`
(part of MAME): `chdman createcd -i RidgeRacerV.cue -o RidgeRacerV.chd`. The project cannot help with finding
game files and will not link to them.

## Known issues

- Only the USA release is supported (`SLUS_200.02`; the app checks the disc's SHA-256 hashes).
- A long live session with many cars on screen stalled in one early test (a replay of the same session ran fine);
  the cause is not identified yet.
- No in-game controller remapping yet; the default layout is in the user guide.
- Audio is verified by listening and by measured tests, not by a full hardware comparison.

More in the release notes and on the [issue tracker](https://github.com/danisandoval/fukami/issues).

## Building from source

The repository holds the tooling and the runtime; the game code is **generated from your own ELF** by
`scripts/fukami_generate.py` and never committed. See [BUILDING.md](BUILDING.md). How it fits together is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); how we check changes is in [docs/TESTING.md](docs/TESTING.md).

## Contributing

Issues and pull requests are welcome; read [CONTRIBUTING.md](CONTRIBUTING.md) first. We do not accept game assets,
copyrighted material, help with piracy, or copy-protection work.

## Credits

Fukami builds on the work of many people and projects:

- [PCSX2](https://github.com/PCSX2/pcsx2) (GPL-3.0+): the GS renderer and SPU2 core.
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j (GPL-3.0): the recompiler and runtime this project started from.
- [SDL](https://libsdl.org/), [libchdr](https://github.com/rtissera/libchdr), [sse2neon](https://github.com/DLTcollab/sse2neon),
  [Dear ImGui](https://github.com/ocornut/imgui) and the other libraries listed in
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- The recompilation community, whose public projects ([N64Recomp](https://github.com/N64Recomp/N64Recomp),
  [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and others) were a big inspiration for this project.

## Licence

Fukami is released under the **GNU General Public License v3.0** ([LICENSE](LICENSE)). PCSX2-derived code
keeps its upstream notices. The licence covers this project's source code only; Ridge Racer V is the property of its
owners.
