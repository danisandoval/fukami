# Changelog

Versions follow [Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`. The file `VERSION` holds the current one.

## Unreleased

- 2D (HUD, menus, text, logos) keeps even pixels at every internal resolution, and the 4:3 HUD on a wide picture (`hud = fixed` or `race`) no longer shows pieces of the neighbouring letter (a bar after "POSITION", doubled colons, a mark inside the "0" of the lap record). The title screen's logos, PRESS START BUTTON and the copyright line no longer end in a stray line at higher internal resolutions. New setting `sprite_filter` (menu: 2D filtering): `even` (default) or `ps2` for the stock sampling.
- Mipmaps are off by default (setting `mipmap`, menu: Mipmaps). A settings file that already has the line keeps its value.
- The widescreen HUD default is `race` (setting `hud`, menu: HUD): during a race the HUD keeps its 4:3 proportions, pinned to the screen edges; menus stay stretched. It was `stretch`. A settings file that already has the line keeps its value.
- Known issue (widescreen HUD, `hud=fixed` or `race`): on the RETIRE result screen the RETRY / QUIT GP menu is drawn about 80 px to the left for its first visible frame, then jumps to its place. Cosmetic and presentation only.
- Depth of field (the blurred background in the attract, on the memory-card "Now saving" screen and in the fly-by before a race): it keeps its 1x width at internal scales above 1 (it was 1/scale as wide, so sharper). Not applied at scale 1.
- Widescreen: the main menu's character is drawn with the wide view and the dot background is filled at the left, where the 4:3 lattice left a dot-less band.
- Two controllers: the first controller you connect is player 1 and the second is player 2 (a third is ignored). The keyboard plays
  player 1 only while no controller is connected as player 1. If player 1's controller is unplugged, player 2 keeps theirs only while a third controller is waiting; with
  exactly two controllers, player 2's becomes player 1 (the pads swap) and the controller that comes back is player 2.

## 1.0.1 (2026-10-05)

- New setting `texture_filter` (menu: Texture filtering): `bilinear` smooths the car textures, which the game draws
  point-sampled; `ps2` (default) is the stock look.
- Disc images: a `.cue`/`.bin` pair (or a bare `.bin`) works as well as a CHD, at the first-launch prompt and as a command-line argument.
- New setting `logging` (menu: Write logs), off by default. Off, Fukami creates no `sessions/` folder and writes no file while it runs (no
  logs, timing files, pad record or memory card copy). Turn it on before reporting a bug.
- Faster loading on the Steam Deck: the game's data unpacker runs on host pointers, in the background where possible.
- Steam Deck: the GS thread spends about 4% less time in the busiest part of a race (texture hashing no longer copies every block; the images are identical).
- Steam Deck: at internal resolution 3x the start of a race no longer drops below 60 fps. With "Even frame pacing" on (the default), the
  wait for the next screen image no longer holds up the render thread; the game itself does not change.
- Steam Deck: the Internal resolution setting offers even values only (2x, 4x, 6x, 8x); a 3x from an older settings file runs as 2x. At 3x the Deck's GPU drew car panels hatched and
  made car reflections flicker.
- New menu page Experimental with `car_depth_fix` (Car paint depth fix), off by default: removes that hatch at odd resolutions, but
  leaves dark patches on the car in the selection screen.
- In fullscreen the mouse pointer hides after 2 seconds without moving; it returns when the mouse moves or the menu opens.

## 1.0.0 (2026-10-04)

First public release: macOS (Apple silicon) and Steam Deck (SteamOS) builds of Ridge Racer V (USA).

- Native 60 fps progressive output with exact PS2 field timing for the game logic.
- Internal resolution up to 8x, AA1, FXAA, CAS, anisotropic filtering, mipmaps.
- Real widescreen (16:9, 16:10, 21:9), car LOD and draw-distance enhancements, faster loading.
- Controller (with rumble) and keyboard; settings menu; memory card saves.
- VU1 and GS run on two threads by default (`split_gs`) and the frame pacer sleeps (`pacer_spin = 0`) on every platform; both are in the menu.
- Game code generated locally from your own ELF (`scripts/fukami_generate.py`).
