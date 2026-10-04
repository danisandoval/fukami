# Changelog

Versions follow [Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`. The file `VERSION` holds the current one.

## 1.0.0 (2026-10-04)

First public release: macOS (Apple silicon) and Steam Deck (SteamOS) builds of Ridge Racer V (USA).

- Native 60 fps progressive output with exact PS2 field timing for the game logic.
- Internal resolution up to 8x, AA1, FXAA, CAS, anisotropic filtering, mipmaps.
- Real widescreen (16:9, 16:10, 21:9), car LOD and draw-distance enhancements, faster loading.
- Controller (with rumble) and keyboard; settings menu; memory card saves.
- VU1 and GS run on two threads by default (`split_gs`) and the frame pacer sleeps (`pacer_spin = 0`) on every platform; both are in the menu.
- Game code generated locally from your own ELF (`scripts/fukami_generate.py`); releases ship it compiled.
