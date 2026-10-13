#!/usr/bin/env python3
"""Assemble the Fukami Windows x86-64 app folder from a product build.

Usage (from a shell with Python 3.11+; the build is the --build-root of scripts/build_product_runtime.py):
  python scripts/package_fukami_windows.py --build build-product-NAME [--version 0.3.0]
      [--output local/app] [--workload config/product] [--windowed]

Layout (what src/app/fukami_app_win.cpp and src/host/rrv_resource_package_win.cpp expect):

  Fukami/
    Fukami.exe                      <build>/candidate-bin/Fukami.exe
    SDL2.dll, z.dll, zstd.dll       the executable's own DLLs (<build>/candidate-bin/*.dll)
    resources/.rrv-resource-package.lock
    resources/.rrv-resource-packages/current/   (byte-identical copy: the GS bridge, its DLLs, PCSX2 resources)
    share/fukami-default.ini        rrv.ini with the app defaults
    share/workload/bound-workload.txt
    share/LICENSE.txt

This is the first cut of the Windows packager: it writes the folder the app runs from. It does not yet check the DLL
import closure, bundle the Visual C++ runtime (VCRUNTIME140/MSVCP140 must be installed) or write a zip.

The output holds the compiled game code (owner option A, 2026-09-29): it goes to an ignored folder and is never
committed. No disc data, ELF/IRX or captures are copied; the script refuses if any appears in the tree.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_NAMES = re.compile(r'(?i)(\.(elf|irx|iso|chd|bin|img|gsr|gs|irf)$|^SLUS_|^R5\.ALL$|^SYSTEM\.CNF$)')

sys.path.insert(0, str(ROOT / 'scripts'))
from package_fukami_app import default_ini  # noqa: E402  (same ini defaults as the Mac app)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--build', type=Path, required=True, help='product build root (holds candidate-bin/)')
    parser.add_argument('--version', default=(ROOT / 'VERSION').read_text(encoding='utf-8').strip(),
                        help='default: the VERSION file')
    parser.add_argument('--output', type=Path, default=ROOT / 'local/app')
    parser.add_argument('--workload', type=Path,
                        default=(ROOT / 'local/gate3/product-live-v1'
                                 if (ROOT / 'local/gate3/product-live-v1/bound-workload.txt').is_file()
                                 else ROOT / 'config/product'),
                        help='directory holding bound-workload.txt (default: your local one, else config/product)')
    parser.add_argument('--windowed', action='store_true',
                        help='keep `fullscreen = false` in the default ini (development runs)')
    args = parser.parse_args()

    build = args.build.resolve()
    exe = build / 'candidate-bin/Fukami.exe'
    packages = build / '.rrv-resource-packages'
    if not exe.is_file() or not (packages / 'current').is_dir():
        sys.exit(f'{build} is not a finished Windows product build (candidate-bin/Fukami.exe, .rrv-resource-packages)')
    bound = args.workload / 'bound-workload.txt'
    if 'storage persistent_user_card' not in bound.read_text(encoding='utf-8').splitlines():
        sys.exit(f'{bound} is not a product workload')

    app = args.output.resolve() / f'{args.version}-windows' / 'Fukami'
    if app.exists():
        shutil.rmtree(app)
    (app / 'share/workload').mkdir(parents=True)
    (app / 'resources').mkdir()
    for item in sorted((build / 'candidate-bin').iterdir()):
        if item.suffix.lower() in ('.exe', '.dll'):
            shutil.copy2(item, app / item.name)
    (app / 'resources/.rrv-resource-package.lock').touch()
    # The reader refuses reparse points, so the package is copied, never linked.
    shutil.copytree(packages, app / 'resources/.rrv-resource-packages', symlinks=False)
    shutil.copy2(bound, app / 'share/workload/bound-workload.txt')
    ini = default_ini()
    if args.windowed:
        ini = re.sub(r'(?m)^fullscreen = .*$', 'fullscreen = false', ini)
    (app / 'share/fukami-default.ini').write_bytes(ini.encode('utf-8'))
    shutil.copy2(ROOT / 'LICENSE', app / 'share/LICENSE.txt')

    forbidden = [str(p.relative_to(app)) for p in app.rglob('*') if p.is_file() and FORBIDDEN_NAMES.search(p.name)]
    if forbidden:
        shutil.rmtree(app)
        sys.exit('refusing to package game data: ' + ', '.join(forbidden[:10]))
    size = sum(p.stat().st_size for p in app.rglob('*') if p.is_file())
    print(f'{app}  ({size / 1e6:.1f} MB)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
