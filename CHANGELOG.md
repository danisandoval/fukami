# Changelog

Versions follow [Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`. The file `VERSION` holds the current one.

## 1.0.1 (2026-10-05)

- New setting `texture_filter` (menu: Texture filtering): `bilinear` smooths the car textures, which the game draws
  point-sampled; `ps2` (default) is the stock look.
- Disc images: a `.cue`/`.bin` pair (or a bare `.bin`) works as well as a CHD, at the first-launch prompt and as a command-line argument.
- New setting `logging` (menu: Write logs), off by default. Off, Fukami creates no `sessions/` folder and writes no file while it runs (no
  logs, timing files, pad record or memory card copy). Turn it on before reporting a bug.
- Faster loading on the Steam Deck: the game's data unpacker runs on host pointers, in the background where possible.
- Steam Deck: the GS thread spends about 4% less time in the busiest part of a race (texture hashing no longer copies every block; the images are identical).
- Faster VU0 and libvu0 maths on Apple silicon (same results); the native code list keeps its 29 functions.
- In fullscreen the mouse pointer hides after 2 seconds without moving; it returns when the mouse moves or the menu opens.

## 1.0.0 (2026-10-04)

First public release: macOS (Apple silicon) and Steam Deck (SteamOS) builds of Ridge Racer V (USA).

- Native 60 fps progressive output with exact PS2 field timing for the game logic.
- Internal resolution up to 8x, AA1, FXAA, CAS, anisotropic filtering, mipmaps.
- Real widescreen (16:9, 16:10, 21:9), car LOD and draw-distance enhancements, faster loading.
- Controller (with rumble) and keyboard; settings menu; memory card saves.
- VU1 and GS run on two threads by default (`split_gs`) and the frame pacer sleeps (`pacer_spin = 0`) on every platform; both are in the menu.
- Game code generated locally from your own ELF (`scripts/fukami_generate.py`); releases ship it compiled.
