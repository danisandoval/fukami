# Legal notes

**Fukami is not affiliated with, authorised, sponsored or endorsed by Bandai Namco Entertainment, Sony Interactive
Entertainment or any other rights holder.** *Ridge Racer* and *PlayStation* are trademarks of their respective owners;
they are used here only to say which game this software works with.

## What this project is

Fukami is a compatibility layer: a runtime plus tools that let a Ridge Racer V binary that **you own** run natively.
It is not an emulator and it does not run other games.

## What this repository and its releases contain

- Source code written for this project, and code derived from GPL-licensed projects (PCSX2, PS2Recomp), under the
  GNU General Public License v3.0 ([LICENSE](LICENSE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
- Release builds also contain the game's code **translated into native machine code** by the recompiler. A release never
  contains disc data, the original executable, the IOP modules, textures, models, audio, videos or any other asset, and
  no PS2 BIOS: the kernel is reimplemented.
- The source repository contains no game code at all. It is generated on your computer from your own disc by
  `scripts/fukami_generate.py` and kept in an ignored folder.

## What you must provide

- A legally obtained copy of Ridge Racer V (USA), dumped by you from your own disc. Fukami verifies the disc's files by
  SHA-256 and refuses anything else. Do not ask for, offer or link to game files in this project's issues, discussions or
  pull requests; such posts are removed.
- Fukami contains no copy-protection circumvention and the project does not accept work on it.

## Takedown and contact

If you represent a rights holder and believe something in this repository should not be here, please open a private
security advisory on GitHub or contact the maintainer listed on the repository profile; we will respond promptly.
