#!/usr/bin/env python3
"""Re-extract the game C++ from the user's ELF with the recompiler built from third_party/ps2recomp.

Only for a recompiler bug fix or a new hook point (generated/rr5/README.md). The product build
never runs this. Steps (the accounted generation procedure, with the guest rand stub removed
from the stub list so the guest's own srand/rand bodies are generated):

  1. cmake -S . -B build-recompiler -DRRV_BUILD_RECOMPILER=ON && \\
     cmake --build build-recompiler --target ps2_analyzer ps2_recomp
  2. python3 scripts/reextract_game_source.py --recompiler-build build-recompiler \\
         --elf local/rrv_boot.elf --run-dir <new dir>
  3. Compare, then (for a deliberate change) replace generated/rr5/output in one "regenerate" commit,
     refresh the manifests, and run the full acceptance (traces, digests, captures).

By default this only writes <run-dir>/output and reports whether it equals the committed
generated/rr5/output (exit 0 identical, 1 different).
"""
from __future__ import annotations

import argparse
import filecmp
import hashlib
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from generate_rr5_reconstructed import materialize_config  # noqa: E402

GUEST_RAND_STUB = '  "rand@0x002D5F40",\n'


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--recompiler-build", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True, help="the user-owned SLUS_200.02 / rrv_boot.elf")
    parser.add_argument("--run-dir", type=Path, required=True, help="new directory for config, logs and output")
    args = parser.parse_args()
    run = args.run_dir.resolve()
    if run.exists() and any(run.iterdir()):
        raise SystemExit(f"run directory must be new or empty: {run}")
    analyzer = args.recompiler_build.resolve() / "ps2recomp/ps2xAnalyzer/ps2_analyzer"
    recompiler = args.recompiler_build.resolve() / "ps2recomp/ps2xRecomp/ps2_recomp"
    elf = args.elf.resolve()
    for required in (analyzer, recompiler, elf):
        if not required.is_file():
            raise SystemExit(f"missing: {required}")
    (run / "output").mkdir(parents=True, exist_ok=True)
    before = sha256(elf)
    with (run / "analyzer.log").open("w") as log:
        subprocess.run([str(analyzer), str(elf), str(run / "analyzer.toml")],
                       stdout=log, stderr=subprocess.STDOUT, check=True, cwd=ROOT)
    config = run / "rrv.toml"
    materialize_config(elf, run / "output", config)
    text = config.read_text()
    if text.count(GUEST_RAND_STUB) != 1:
        raise SystemExit("effective config must list the rand stub exactly once")
    config.write_text(text.replace(GUEST_RAND_STUB, "", 1))
    with (run / "recompiler.log").open("w") as log:
        subprocess.run([str(recompiler), str(config)], stdout=log, stderr=subprocess.STDOUT,
                       check=True, cwd=ROOT)
    if sha256(elf) != before:
        raise SystemExit("the ELF changed during extraction")
    committed = ROOT / "generated/rr5/output"
    new = sorted(p.name for p in (run / "output").iterdir())
    if not committed.is_dir():  # a public checkout: nothing to compare with (fukami_generate.py verifies hashes)
        print(f"re-extracted {len(new)} files into {run / 'output'}; no generated/rr5/output to compare with")
        return 0
    old = sorted(p.name for p in committed.iterdir())
    same = new == old and all(filecmp.cmp(run / "output" / n, committed / n, shallow=False) for n in new)
    print(f"re-extracted {len(new)} files into {run / 'output'}; "
          f"{'IDENTICAL to' if same else 'DIFFERS from'} generated/rr5/output")
    return 0 if same else 1


if __name__ == "__main__":
    raise SystemExit(main())
