# Roadmap

**1.0 (macOS Apple silicon, Steam Deck):** the first public release; see the [changelog](../CHANGELOG.md).

Planned, in no firm order:

- **Windows (x86-64):** a Vulkan/D3D12 path through the same PCSX2 GS bridge, MSVC/clang-cl builds of the generated code,
  Windows paths and file locking in the runtime, a Windows release.
- **Developer ID signing and notarisation** for the macOS build.
- **Flatpak / AppImage** packaging for desktop Linux.
- **Controller remapping** in the in-game menu.
- More Steam Deck performance work (the heavy race stretches are VU1-bound).
- A full live-session soak (30 minutes, many cars) with measured audio underruns.

Not planned: other games, other regions (until someone can test them), anything that touches copy protection.
