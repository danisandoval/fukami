# Changelog

Versions follow [Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`. The file `VERSION` holds the current one.

## 1.1.0 (2026-10-14)

### Fukami comes to Windows

Ridge Racer V now runs natively on Windows PCs (x86-64), next to macOS and the Steam Deck. Same game, same settings menu, same `rrv.ini`.

- **Direct3D 11 by default**, with Vulkan one setting away (`backend`, menu: Graphics API) for drivers where Direct3D 11 misbehaves.
- **Smooth on any refresh rate.** In fullscreen Fukami switches the display to a multiple of 60 Hz it supports (165 Hz goes to 120, 144 to 120, 75 to 60) and back on leaving (`refresh_rate`, default `auto`).
- **FreeSync and G-Sync.** On a variable-refresh display every picture is shown for exactly 16.7 ms, whatever the display's rate (`vrr`, default `auto`: used once the display is seen following the game).
- Everything else from macOS and the Deck: internal resolution up to 8x, real widescreen, AA, the depth-of-field effects at their intended resolution, controllers with rumble, memory card saves.

### Picture

- 2D (HUD, menus, text, logos) keeps even pixels at every internal resolution. The 4:3 HUD on a wide picture (`hud = fixed` or `race`) no longer shows pieces of the neighbouring letter (a bar after "POSITION", doubled colons, a mark inside the "0" of the lap record).
- The title screen's logos, PRESS START BUTTON and copyright line no longer end in a stray line at higher internal resolutions.
- New setting `sprite_filter` (menu: 2D filtering): `even` (default) or `ps2` for the stock sampling.
- Mipmaps are off by default (setting `mipmap`, menu: Mipmaps). At the resolutions Fukami plays at, they mostly blur distant textures. A settings file that already has the line keeps its value.
- Depth of field looks as the game intended at every internal resolution.

### Widescreen

- The widescreen HUD default is now `race` (setting `hud`, menu: HUD): during a race the HUD keeps its 4:3 proportions, pinned to the screen edges; menus stay stretched. It was `stretch`. A settings file that already has the line keeps its value.
- The intro renders full screen in 16:9.

### Controllers

- Two players: the first controller you connect is player 1 and the second is player 2 (a third is ignored).
- The keyboard plays player 1 only while no controller is connected as player 1.
- With exactly two controllers, unplugging player 1's makes player 2's player 1, and the controller that comes back is player 2. A pad that shows up twice (Bluetooth and USB) is never stuck as player 2.

### Fixes

- VS mode no longer goes to a black screen.

### Steam Deck

- The Internal resolution setting offers even values only (2x, 4x, 6x, 8x); a 3x from an older settings file runs as 2x. At 3x the Deck's GPU drew car panels hatched and made car reflections flicker.
- New menu page Experimental with `car_depth_fix` (Car paint depth fix), off by default. Causes some gfx glitches.

### Known issues

- Widescreen HUD (`hud = fixed` or `race`): on the RETIRE result screen the RETRY / QUIT GP menu is drawn about 80 px to the left for its first visible frame, then jumps to its place. Cosmetic only.
- `car_depth_fix` (Experimental, off by default): part of the hood of the car sliding into the car selection screen turns black.

## 1.0.1 (2026-10-05)

- New setting `texture_filter` (menu: Texture filtering): `bilinear` smooths the car textures, which the game draws
  point-sampled; `ps2` (default) is the stock look.
- Disc images: a `.cue`/`.bin` pair (or a bare `.bin`) works as well as a CHD, at the first-launch prompt and as a command-line argument.
- New setting `logging` (menu: Write logs), off by default. Off, Fukami creates no `sessions/` folder and writes no file while it runs (no
  logs, timing files, pad record or memory card copy). Turn it on before reporting a bug.
- Faster loading on the Steam Deck: the game's data unpacker runs on host pointers, in the background where possible.
- Steam Deck: the GS thread spends about 4% less time in the busiest part of a race (texture hashing no longer copies every block; the images are identical).
- In fullscreen the mouse pointer hides after 2 seconds without moving; it returns when the mouse moves or the menu opens.

## 1.0.0 (2026-10-04)

First public release: macOS (Apple silicon) and Steam Deck (SteamOS) builds of Ridge Racer V (USA).

- Native 60 fps progressive output with exact PS2 field timing for the game logic.
- Internal resolution up to 8x, AA1, FXAA, CAS, anisotropic filtering, mipmaps.
- Real widescreen (16:9, 16:10, 21:9), car LOD and draw-distance enhancements, faster loading.
- Controller (with rumble) and keyboard; settings menu; memory card saves.
- VU1 and GS run on two threads by default (`split_gs`) and the frame pacer sleeps (`pacer_spin = 0`) on every platform; both are in the menu.
- Game code generated locally from your own ELF (`scripts/fukami_generate.py`).
