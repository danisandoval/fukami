# Fukami user guide

Fukami plays Ridge Racer V (USA, `SLUS-20002`) natively on macOS (Apple silicon) and on the Steam Deck.
**No game data is included: you need your own copy of the game.** Fukami is not affiliated with or endorsed by
Bandai Namco or Sony.

## What you need

- A Mac with Apple silicon (M1 or later), or a Steam Deck (SteamOS 3.7+).
- Your own copy of Ridge Racer V (USA) as a **CHD** file, made from your disc (see the README, "Getting your CHD").
- About 500 MB of free space for the game files, plus your saves.
- Optional: a game controller (DualShock 4/5, Xbox or any SDL-supported pad).

Only the USA disc is supported. A different release or a damaged dump is refused with a message; nothing is written.

## macOS

1. Unzip `Fukami-<version>-macos-arm64.zip` and drag **Fukami** to Applications.
2. The first time, macOS asks before opening an app that is not from the App Store. Right-click Fukami and choose
   **Open**, then **Open** again. (Or open it once, then System Settings → Privacy & Security → **Open Anyway**.)
   Fukami is signed ad-hoc and not yet notarised, so this step is expected.
3. Fukami asks for your CHD. Click **Choose CHD…** and pick the file. It checks the disc and unpacks the game
   files once, with a progress bar (about half a minute). You can delete the CHD afterwards.
4. The game starts fullscreen.

## Steam Deck

1. In Desktop Mode, copy `Fukami-<version>-linux-x86_64.tar.gz` and your `.chd` to the Deck. Right-click the
   archive and choose Extract.
2. Open a terminal in the extracted `Fukami` folder and run `./install.sh`. It copies Fukami to
   `~/Applications/Fukami`, adds a menu entry and adds Fukami to Steam as a non-Steam game.
3. Put your `.chd` in `~/.local/share/Fukami/` (or start Fukami once from Desktop Mode and pick it). The first start
   unpacks it once.
4. Switch to Game Mode and start **Fukami** from your library.

The Deck build uses 2x internal resolution by default. If the picture stutters, try **Pacer spin** (Performance in the
menu, or `pacer_spin` under `[timing]`; 0 by default): a value up to 17 keeps one CPU core busy between frames so the
Deck does not slow its CPU down, at the cost of battery.

## Controls

| PS2 | Controller | Keyboard |
|---|---|---|
| Cross / Circle / Square / Triangle | A / B / X / Y | Space / Backspace or C / Z or Keypad 0 / X or Keypad 1 |
| D-pad | D-pad | Arrow keys or WASD |
| L1 / R1 | Shoulders | Q / E |
| L2 / R2 | Analog triggers | Left Shift / Right Shift |
| Start / Select | Start / Back | Return / Tab |
| Sticks, L3 / R3 | Sticks (rumble supported) | |
| Menu | Guide / Home button | Esc or F1 |
| Fullscreen | | F11 |

There is no in-game remapping yet.

## The menu and settings

Esc, F1 or the controller's Guide button opens the settings menu over the game. Changes are saved at once; settings
marked *restart* apply after **Restart** in the menu. Everything the menu changes is in `fukami.ini` (below), so you can
also edit it while Fukami is closed.

Main options: aspect **ratio** (real widescreen), **scale** (internal resolution), anti-aliasing (AA1, FXAA, CAS), anisotropic
filtering, mipmaps, **car LOD**, **draw distance**, rumble, analog mode, fullscreen, and a Performance section.

**Quit:** Cmd+Q on macOS, the menu's **Quit**, or close the window. Saving is safe even if power is lost mid-save; the
previous save is kept.

## Your files

| | macOS | Linux / Steam Deck |
|---|---|---|
| Folder | `~/Library/Application Support/Fukami/` | `~/.local/share/Fukami/` |

| Item | What it is |
|---|---|
| `fukami.ini` | Your settings (the menu writes it; you can edit it while Fukami is closed) |
| `mc/` | Your memory card (saves) |
| `disc/` | The unpacked game files |
| `sessions/` | One log folder per play session |

A setting missing from `fukami.ini` (for example one added by a newer version) uses the app's default; Fukami does not
rewrite your file for that. The menu has **Show saves** and **Show logs**. To start over, quit Fukami and delete the
folder (your saves go with it).

### Advanced: environment variables

`fukami.ini` has an `[env]` section for advanced users. Only a short list of names is accepted (documented in the
comments of the file). Any other `RRV_` variable is for debugging the game itself and needs `enabled = true` under
`[developer]`. Without it, Fukami tells you which line is the problem and offers to reset the file. Some of those variables
write files into your folders; leave developer mode off unless you know why you are turning it on.

## Troubleshooting

- **"This CHD can't be used":** it is not the USA disc, or the dump is damaged. Re-dump it.
- **macOS refuses to open the app:** see step 2 above.
- **Stutter on the Deck:** try Pacer spin (set it to 17 under Performance; it uses more battery), and check that the Deck is not in a low power limit.
- **A crash or odd behaviour:** open **Show logs** in the menu and attach the newest `sessions/` folder to an issue
  (it contains no game data).

## Known issues

See the release notes and the [issue tracker](https://github.com/danisandoval/fukami/issues) for the current list.
