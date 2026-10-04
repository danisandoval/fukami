# Testing

## Asset-free suite (runs on a clean checkout, no game data)

```sh
cmake --preset m0-macos-arm64            # on Linux: m0-linux-x86_64
cmake --build build/m0-macos-arm64 --target rrv-asset-free-tests --parallel
python3 scripts/qualify_asset_free.py --build-dir build/m0-macos-arm64
```

`scripts/asset_free_suite.json` lists the tests (`required`, plus the ones that need local inputs such as a built product
or the pinned PCSX2 checkout). A required test that is missing or skipped fails the run. Tests that need the generated game
code skip themselves until you run `scripts/fukami_generate.py`. The preset names are historical.

CI runs the suite on macOS and Linux and checks that the PCSX2 patch series reproduces the locked tree
(`python3 scripts/pcsx2_patch_series.py check --pcsx2-source <checkout>`).

## What the suite does not prove

It builds no product and renders nothing. A change that claims *no behaviour change* has to be checked against the
runtime it replaces, with the same workload, on your own game data: traced runs, per-start digests and pixel captures
compared byte for byte, after a control run of the unchanged runtime against itself (a comparison without a control
measures the window, not the change). Debug by **first divergence** against PCSX2 as the reference for intended PS2
behaviour rather than by looking at final images.

State what you actually ran: configured, assumed, observed or verified, and report skipped checks as skipped.
