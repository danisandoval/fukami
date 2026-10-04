# Building Fukami

You do not need to build anything to play: download a release. This page is for contributors and for anyone who wants
to build from source. **The repository contains no game code**: you generate it from your own `SLUS_200.02` (step 3).

| Platform | Build |
|---|---|
| macOS (Apple silicon) | native, steps 1–6 below |
| Steam Deck / Linux x86-64 | in a Linux container, [step 7](#7-steam-deck--linux) |
| Windows | not yet; see the [roadmap](docs/ROADMAP.md) |

## 0. Tools

- **macOS:** Xcode with the Metal toolchain (`xcodebuild -downloadComponent MetalToolchain`), CMake ≥ 3.21, Ninja, Python 3.9+,
  Git, and the libraries PCSX2's bridge links, from Homebrew:
  `brew install cmake ninja pkgconf python sdl3 rapidyaml freetype jpeg-turbo libpng webp lz4 zstd qt`.
  (CMake names any other missing package when it configures the bridge.)
- About 4 GB of free disk space for the dependency checkouts and the builds, and a network connection for the first run
  (it clones PCSX2 and sse2neon and downloads SDL and the recompiler's pinned libraries).

## 1. Check the checkout without any game data

```sh
cmake --preset m0-macos-arm64
cmake --build build/m0-macos-arm64 --target rrv-asset-free-tests --parallel
python3 scripts/qualify_asset_free.py --build-dir build/m0-macos-arm64
```

This runs 32 tests, none of which need game data (see [docs/TESTING.md](docs/TESTING.md)). The `m0-` preset names are historical.

## 2. Fetch the pinned dependencies

```sh
python3 scripts/prepare_dependencies.py --prepare
python3 scripts/prepare_sdl.py --prepare
python3 scripts/prepare_dependencies.py --verify
```

Revisions and patch hashes are in `config/dependencies.lock.toml` (PS2Recomp is vendored in `third_party/`, so there is nothing to fetch for it). PCSX2 is cloned clean into `build-deps/` and is never
modified in place: the bridge build applies the ordered series in `tools/patches/pcsx2/` to a private copy.

## 3. Generate the game code from your ELF

```sh
python3 scripts/fukami_generate.py --elf /path/to/SLUS_200.02
```

`SLUS_200.02` is the boot file of your Ridge Racer V (USA) disc. If you have already started Fukami.app once, it
unpacked that file to `~/Library/Application Support/Fukami/disc/`, and `--elf` can be left out.

This builds the recompiler from `third_party/ps2recomp`, runs it, generates the ahead-of-time VU code and the hot-function
file, and writes everything to the ignored directory `generated/rr5/`. It then checks every file against the reference
hashes in `config/rr5/`: **a different ELF, region or generator version fails**, and nothing half-written is left in place.
It takes about a minute plus the recompiler build. The generated code is a derivative of the game; never commit it or
share it (`.gitignore` and the CI gates keep it out of the repository).

## 4. Build the runtime

The runtime build works from a *committed* revision, so commit your changes first (the build never uses a dirty tree).

```sh
scripts/build_fukami_runtime.sh HEAD fukami-1.0.0 v1
```

Arguments: the revision, a name for the runtime package (`runtime/<name>`), and a tag for the build directories. It
builds PCSX2's Metal libraries (first run only), the PCSX2 GS bridge from the pinned source plus the patch series
(about 10 minutes), then the product (`RRV_BUILD_PRODUCT=ON`, `RRV_PRODUCT_OWNED_SOURCE=ON`: the generated code, the
override layer and the runtime are compiled in place), and packages and verifies the result. Everything lands in ignored
directories (`build-*/`, `runtime/`).

Where things go: all build products are under **`build/fukami/`** (`metal/`, `root-<commit>/`, `bridge-<tag>-<commit>/` and the
patched PCSX2 source beside it) plus `runtime/`; the pinned dependency checkouts are `build-deps/` and `runtime-deps/`.
All are ignored by git and none is shipped (a release takes only the finished libraries out of `runtime/<name>`). The
`bridge-*-source-*` directory is the exact patched PCSX2 tree that the bridge's provenance manifest records; keep it for as
long as you keep that bridge. To reclaim space: `rm -rf build/fukami` (the next build redoes the bridge, about 10 minutes).

Environment: `FUKAMI_BUILD_DIR` (replaces `build/fukami`), `FUKAMI_JOBS` (parallel jobs, default 8), `FUKAMI_METAL_RESOURCES` (reuse a Metal-resources directory),
`FUKAMI_MACOS_TARGET` (the oldest macOS to support; set the same value for the bridge and the product, which this script
does; without it the build targets the macOS it runs on).

## 5. Make the app

```sh
python3 scripts/package_fukami_app.py --runtime fukami-1.0.0 --zip
```

Writes `local/app/<VERSION>/Fukami.app` and a zip (the version comes from the `VERSION` file unless `--version` is given).
The app is signed ad-hoc; it bundles the libraries from Homebrew that the bridge needs and the licence notices. It
asks for a CHD on first launch like a release does.

## 6. Check a change

Run the asset-free suite (step 1). A change that claims to leave the game's behaviour alone must also reproduce, byte for
byte, what the runtime it replaces does on your own game data: record the per-start RAM digests (`RRV_GATE3_OBSERVE_DIGEST=1`)
and the semantic trace of a control build and of your build on the same workload, then compare. Details and the rationale
are in [docs/TESTING.md](docs/TESTING.md). Run at most two game instances at once.

## 7. Steam Deck / Linux

The Linux build runs in a container so that the result runs on SteamOS (Ubuntu 24.04 userland, glibc 2.39). It needs Docker
and, on Apple silicon, Rosetta emulation for `linux/amd64` (the bridge takes about 25 minutes).

```sh
docker build --platform linux/amd64 -t rrv-linux-build:1 tools/linux-build        # once; builds PCSX2's dependencies
tools/linux-build/build_linux_runtime.sh HEAD fukami-1.0.0-linux-x86_64          # bridge, product, runtime
docker run --rm --platform linux/amd64 -v "$PWD":"$PWD" -w "$PWD" -e HOME=/tmp/rrv-home rrv-linux-build:1 \
    python3 scripts/package_fukami_linux.py --runtime fukami-1.0.0-linux-x86_64
```

The package is `local/app/<VERSION>-linux/Fukami-<VERSION>-linux-x86_64.tar.gz`: the app, its libraries, `install.sh`
and the Steam Deck defaults. Do steps 2–3 on the host first (the container sees the same checkout).

## Layout

| Path | What |
|---|---|
| `src/product`, `src/app` | override layer (all game-behaviour changes), the app (disc extraction, settings, menu) |
| `third_party/ps2recomp` | recompiler, analyzer and runtime (PS2Recomp-derived; `RRV_CHANGES.md`) |
| `src/guest-time`, `src/vu-aot`, `src/gs-backend`, `src/audio`, `src/hle` | guest time, VU code, GS ABI, audio, kernel services |
| `tools/pcsx2-gs-bridge`, `tools/pcsx2-spu2`, `tools/patches/pcsx2` | the PCSX2 bridge and its patch series |
| `tools/vu-aot`, `tools/ee-native` | generators for the VU code and the hot functions |
| `config/` | recompiler config, dependency lock, reference hashes (`rr5/`), product workload (`product/`) |
| `generated/` | **local only**: output of `fukami_generate.py` |
| `scripts/`, `cmake/`, `tests/` | build, packaging and test tooling |

Architecture and invariants: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
