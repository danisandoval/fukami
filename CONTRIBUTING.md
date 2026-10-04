# Contributing

Thanks for helping. A few ground rules keep this project legal and maintainable.

## Hard rules

- **No game material.** Never commit, attach or link to game files: disc images, ELF/IRX binaries, extracted assets,
  captures (`.gsr`, `.gs`, VRAM dumps), BIOS images or the generated `generated/rr5/` directory. Issues and PRs that
  contain any are closed and removed. Logs from the app's `sessions/` folder are fine (they contain none).
- **No copy-protection work** and no help obtaining the game illegally.
- **GPL-3.0.** Your contribution is licensed under the project's licence. If you reuse PCSX2 code, keep the upstream
  licence header and say which revision, file and function it comes from.
- **Game behaviour changes only in the override layer** (`src/product/patches.cpp`) or in the runtime
  (`third_party/ps2recomp/ps2xRuntime`), never in generated code. Anything that changes what the game does is an
  explicit, documented, user-selectable enhancement (widescreen, draw distance and the like).
- **Settings:** every user toggle lives in `rrv.ini` and in the in-game menu (one schema, `src/app/fukami_settings.cpp`).

## Working on it

1. Read [BUILDING.md](BUILDING.md) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
2. Run the asset-free suite ([docs/TESTING.md](docs/TESTING.md)) before and after your change.
3. For anything that touches timing, rendering or the runtime, say how you verified it on real game data and what you
   compared against. PCSX2 is the reference for intended PS2 behaviour; find the **first divergence** rather than
   guessing from the final picture.
4. One pull request per change, small logical commits, docs and tests with the code.

## Reporting a bug

Use the bug-report template: version, platform, what you did, the newest `sessions/` log. Do not attach game data.

AI-assisted contributions are welcome if a person has read, run and understood them; say so in the PR.
