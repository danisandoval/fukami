# src/product — the product's own host entry and patch layer

These files are the source of the shipping product (Gate-3/4 runtime):

- `main_product.cpp` — product host entry (SDL host, bound-workload run, terminal outcome).
- `patches.cpp` — the override layer: function replacements, native hot functions,
  RR5 enhancements. **Game behaviour changes go here**, never into `generated/rr5/output`.
- `rr5_car_builder.h` — included by `patches.cpp`: the hand-written native block loop of the
  game's vertex builder func_222EB8 (cars), which the generated native copy of that function
  enters through a generator hook (`tools/ee-native/ee_native_gen.py --hook`). Same guest
  state and guest time as the generated code; `RRV_RR5_NATIVE_BUILDER_VERIFY=1` checks it.

They are ordinary source: edit them directly and commit. The first two began as the stage files of v141 (`2c3f04f`) from the
retired Gate-3 overlay chain (`scripts/historical/`); since then the ELF identity check was added to `main_product.cpp`.
`src/main_product.cpp` and `src/patches.cpp` belong to the legacy/diagnostic builds (`RRV_PRODUCT_OWNED_SOURCE=OFF`);
the default product compiles `src/product/`.

## Game-derived text is not in this tree

`patches.cpp` ends its prelude with `#include "rrv_ee_native.inc"`: the statically derived native hot-function
bodies (`generated/rr5/native/rrv_ee_native.inc`, game-derived, private repo only) are found through the
game-derived include directory (`RRV_GENERATED_INCLUDE_DIR`, default `generated/rr5`). No build step edits or
copies this file.

## Game ELF identity

`main_product.cpp` hashes the ELF it is asked to load (`rrv_elf_identity.h`) and refuses to start (exit 3, with a
message) unless it is the ELF recorded in `generated/rr5/source-manifest.json` (`game_input.sha256`, embedded at
configure time as `RRV_GAME_ELF_SHA256`). `./run.sh` and the Fukami.app first-launch path (which re-executes this
binary with the extracted `SLUS_200.02`) both reach it.
