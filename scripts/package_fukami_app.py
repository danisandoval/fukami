#!/usr/bin/env python3
"""Assemble Fukami.app from a packaged product runtime (Gate-9 APP1).

Usage:
  python3 scripts/package_fukami_app.py --runtime NAME [--version 0.1.0]
      [--output local/app] [--workload local/gate3/product-live-v1] [--zip]

Layout (the runtime's executable becomes the app's main executable; its
startup hook, src/app/fukami_app.mm, does the first-launch setup and
re-executes itself with the launcher environment):

  Fukami.app/Contents/
    Info.plist                      racing-games category + Game Mode
    MacOS/Fukami                    runtime/NAME/bin/Fukami
    Frameworks/                     SDL2 + the GS bridge's non-system dylibs
    Resources/Fukami.icns
    Resources/fukami-default.ini    rrv.ini with the app defaults
    Resources/workload/bound-workload.txt
    Resources/game/.rrv-resource-package.lock
    Resources/game/.rrv-resource-packages/current/   (byte-identical copy)
    Resources/LICENSE-NOTICES.txt

The resource package is copied unchanged: the runtime checks every file's
SHA-256 against the inventory compiled into it, so the bridge dylib cannot be
relinked. Its Homebrew dependencies are found through DYLD_LIBRARY_PATH
(Contents/Frameworks), which the startup hook sets for the game process, as
the developer launcher sets runtime/NAME/lib. The app is signed ad hoc
(owner decision 2026-09-29: free signing, no hardened runtime), which keeps
DYLD_LIBRARY_PATH working.

The output holds the compiled game code (owner option A): it goes to an
ignored folder and is never committed. No disc data, ELF/IRX or captures are
copied; the script refuses if any appears in the tree.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]
BUNDLE_ID = 'io.github.danisandoval.fukami'
SYSTEM_PREFIXES = ('/usr/lib/', '/System/')
FORBIDDEN_NAMES = re.compile(r'(?i)(\.(elf|irx|iso|chd|bin|img|gsr|gs|irf)$|^SLUS_|^R5\.ALL$|^SYSTEM\.CNF$)')


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def dylib_deps(path: Path) -> list[str]:
    lines = run('otool', '-L', str(path)).splitlines()[1:]
    return [line.strip().split(' (')[0] for line in lines if line.strip()]


def rpaths(path: Path) -> list[str]:
    out, found, grab = [], run('otool', '-l', str(path)).splitlines(), False
    for line in found:
        line = line.strip()
        if line == 'cmd LC_RPATH':
            grab = True
        elif grab and line.startswith('path '):
            out.append(line[5:].split(' (offset')[0])
            grab = False
    return out


def resolve(dep: str, loader: Path, loader_rpaths: list[str]) -> Path | None:
    if dep.startswith('@rpath/'):
        for rp in loader_rpaths:
            rp = rp.replace('@loader_path', str(loader.parent))
            candidate = Path(rp) / dep[len('@rpath/'):]
            if candidate.exists():
                return candidate.resolve()
        for guess in (Path('/opt/homebrew/lib'), Path('/usr/local/lib')):
            candidate = guess / dep[len('@rpath/'):]
            if candidate.exists():
                return candidate.resolve()
        return None
    if dep.startswith('@loader_path/'):
        candidate = loader.parent / dep[len('@loader_path/'):]
        return candidate.resolve() if candidate.exists() else None
    candidate = Path(dep)
    return candidate.resolve() if candidate.exists() else None


def bundle_closure(start: Path, frameworks: Path) -> list[str]:
    """Copy every non-system dylib start needs (transitively) into frameworks,
    under the leaf name it is referenced by, and point the copies at each
    other with @loader_path. Returns the copied leaf names."""
    copied: dict[str, Path] = {}
    queue = [(start, dylib_deps(start), rpaths(start))]
    while queue:
        loader, deps, loader_rpaths = queue.pop()
        for dep in deps:
            if dep.startswith(SYSTEM_PREFIXES):
                continue
            leaf = Path(dep).name
            if leaf in copied or leaf == start.name:
                continue
            source = resolve(dep, loader, loader_rpaths)
            if source is None:
                sys.exit(f'cannot resolve {dep} needed by {loader}')
            target = frameworks / leaf
            shutil.copy2(source, target)
            os.chmod(target, 0o755)
            copied[leaf] = target
            queue.append((source, dylib_deps(source), rpaths(source)))
    for leaf, target in copied.items():
        run('install_name_tool', '-id', f'@loader_path/{leaf}', str(target))
        for dep in dylib_deps(target):
            if not dep.startswith(SYSTEM_PREFIXES) and Path(dep).name in copied:
                run('install_name_tool', '-change', dep, f'@loader_path/{Path(dep).name}', str(target))
        for rp in rpaths(target):
            run('install_name_tool', '-delete_rpath', rp, str(target))
    return sorted(copied)


def min_macos(binary: Path) -> str:
    text = run('otool', '-l', str(binary))
    match = re.search(r'minos (\d+(?:\.\d+)*)', text)
    return match.group(1) if match else '14.0'


def default_ini() -> str:
    # The committed rrv.ini, not the working copy (the developer's own menu
    # changes must not become the app's defaults).
    text = run('git', '-C', str(ROOT), 'show', 'HEAD:rrv.ini')
    header_end = text.index('[display]')
    body = text[header_end:]
    # The app owns its folders: drop the developer-only [paths] section.
    body = re.sub(r'\n\[paths\].*?(?=\n\[)', '\n', body, flags=re.S)
    body = re.sub(r'(?m)^fullscreen = .*$', 'fullscreen = true', body)
    header = (
        '# =============================================================================\n'
        '# Fukami settings.\n'
        '#\n'
        '# The in-game menu (Esc or F1) changes these and saves them here. You can\n'
        '# also edit this file while the game is closed. Settings marked "restart"\n'
        '# in the menu apply the next time the game starts.\n'
        '#\n'
        '# Format: `key = value`, one per line. Lines starting with # or ; are comments.\n'
        '# Booleans accept: true/false, yes/no, on/off, 1/0.\n'
        '# A setting that is missing from this file (deleted, commented out, or added\n'
        '# by a newer version of the app) uses the app\'s default.\n'
        '# =============================================================================\n\n\n')
    return header + body


def make_icon(resources: Path) -> None:
    script = ROOT / 'tools/fukami-app/make_icon.py'
    iconset = resources / 'Fukami.iconset'
    subprocess.run([sys.executable, str(script), str(iconset)], check=True)
    run('iconutil', '-c', 'icns', str(iconset), '-o', str(resources / 'Fukami.icns'))
    shutil.rmtree(iconset)


def info_plist(version: str, minimum: str) -> dict:
    return {
        'CFBundleName': 'Fukami',
        'CFBundleDisplayName': 'Fukami',
        'CFBundleIdentifier': BUNDLE_ID,
        'CFBundleExecutable': 'Fukami',
        'CFBundleIconFile': 'Fukami',
        'CFBundlePackageType': 'APPL',
        'CFBundleShortVersionString': version,
        'CFBundleVersion': version,
        'CFBundleInfoDictionaryVersion': '6.0',
        'LSMinimumSystemVersion': minimum,
        'LSArchitecturePriority': ['arm64'],
        'LSRequiresNativeExecution': True,
        # Game Mode (macOS 14+): a game category plus the explicit opt-in.
        'LSApplicationCategoryType': 'public.app-category-racing-games',
        'GCSupportsGameMode': True,
        'NSHighResolutionCapable': True,
        'NSSupportsAutomaticGraphicsSwitching': True,
        'NSHumanReadableCopyright': 'Fukami, GPL-3.0. Not affiliated with Bandai Namco.',
    }


def notices(frameworks: list[str]) -> str:
    lines = [
        'Fukami - a static-recompilation port runtime for Ridge Racer V (USA).',
        'Licence: GNU General Public License v3.0.',
        'You must supply your own copy of the game; no game data is included.',
        'Not affiliated with or endorsed by Bandai Namco.',
        '',
        'Third-party components:',
        '- PCSX2 2.8.2 (GS renderer, SPU2 core, libchdr/lzma): GPL-3.0 and component licences.',
        '- SDL 2.32.10: zlib licence.',
        '- libchdr: BSD-3-Clause. zstd: BSD. lzma SDK: public domain.',
    ]
    lines += [f'- {name}' for name in frameworks]
    return '\n'.join(lines) + '\n'


def credits_html(frameworks: list[str]) -> str:
    """Resources/Credits.html: macOS shows it in the standard About panel
    (SDL's app menu has About Fukami) - Gate-9 APP8."""
    items = ''.join(f'<li>{name}</li>' for name in frameworks)
    return (
        '<html><body style="font-family:-apple-system;font-size:11px;text-align:center">'
        '<p>A native port runtime for Ridge Racer V (USA).<br>'
        'You supply your own copy of the game; no game data is included.<br>'
        'Not affiliated with or endorsed by Bandai Namco.</p>'
        '<p>Licence: GNU General Public License v3.0.<br>'
        'Source code: published with the public release.</p>'
        '<p>Includes PCSX2 2.8.2 (GPL-3.0), SDL 2.32.10 (zlib), libchdr (BSD-3), '
        'zstd (BSD), the LZMA SDK (public domain) and:</p>'
        f'<ul style="list-style:none;padding:0">{items}</ul>'
        '</body></html>\n')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--runtime', required=True, help='package name under runtime/')
    parser.add_argument('--version', default=(ROOT / 'VERSION').read_text().strip() if (ROOT / 'VERSION').is_file() else '0.1.0',
                        help='default: the VERSION file')
    parser.add_argument('--output', type=Path, default=ROOT / 'local/app')
    parser.add_argument('--workload', type=Path,
                        default=(ROOT / 'local/gate3/product-live-v1' if (ROOT / 'local/gate3/product-live-v1/bound-workload.txt').is_file()
                                 else ROOT / 'config/product'),
                        help='directory holding bound-workload.txt (default: your local one, else config/product)')
    parser.add_argument('--zip', action='store_true', help='also write Fukami-VERSION.zip')
    args = parser.parse_args()

    package = ROOT / 'runtime' / args.runtime
    subprocess.run([sys.executable, str(ROOT / 'scripts/package_gate4_product.py'), '--verify',
                    '--name', args.runtime], check=True)
    exe = package / 'bin/Fukami'
    if not exe.is_file():  # a runtime packaged before the executable was renamed
        exe = package / 'bin/rrv-gate3-candidate'
    bound = args.workload / 'bound-workload.txt'
    if 'storage persistent_user_card' not in bound.read_text().splitlines():
        sys.exit(f'{bound} is not a product workload')

    out = args.output.resolve() / args.version
    app = out / 'Fukami.app'
    if app.exists():
        shutil.rmtree(app)
    contents = app / 'Contents'
    macos, frameworks, resources = contents / 'MacOS', contents / 'Frameworks', contents / 'Resources'
    for folder in (macos, frameworks, resources / 'workload', resources / 'game'):
        folder.mkdir(parents=True)

    main_exe = macos / 'Fukami'
    shutil.copy2(exe, main_exe)
    for rp in rpaths(main_exe):
        run('install_name_tool', '-delete_rpath', rp, str(main_exe))
    run('install_name_tool', '-add_rpath', '@executable_path/../Frameworks', str(main_exe))
    shutil.copy2(package / 'lib/libSDL2-2.0.0.dylib', frameworks / 'libSDL2-2.0.0.dylib')
    run('install_name_tool', '-id', '@rpath/libSDL2-2.0.0.dylib', str(frameworks / 'libSDL2-2.0.0.dylib'))

    (resources / 'game/.rrv-resource-package.lock').touch()
    shutil.copytree(package / '.rrv-resource-packages', resources / 'game/.rrv-resource-packages',
                    symlinks=False)
    bridge = resources / 'game/.rrv-resource-packages/current/librrv-pcsx2-gs-bridge.dylib'
    extra = bundle_closure(bridge, frameworks)
    shutil.copy2(bound, resources / 'workload/bound-workload.txt')
    (resources / 'fukami-default.ini').write_text(default_ini())
    (resources / 'LICENSE-NOTICES.txt').write_text(notices(['libSDL2-2.0.0.dylib'] + extra))
    (resources / 'Credits.html').write_text(credits_html(extra))
    shutil.copy2(ROOT / 'LICENSE', resources / 'LICENSE.txt')
    make_icon(resources)
    minimum = min_macos(main_exe)
    with open(contents / 'Info.plist', 'wb') as stream:
        plistlib.dump(info_plist(args.version, minimum), stream)

    # Nothing from the disc may ship.
    for path in app.rglob('*'):
        if FORBIDDEN_NAMES.search(path.name) and path.name not in ('default.metallib',):
            sys.exit(f'refusing to ship {path}')

    # Ad-hoc signatures: nested dylibs first, then the bundle (which signs the
    # main executable). The resource package's dylib keeps its own signature.
    for dylib in sorted(frameworks.iterdir()):
        run('codesign', '--force', '--sign', '-', '--timestamp=none', str(dylib))
    run('codesign', '--force', '--sign', '-', '--timestamp=none', str(app))
    run('codesign', '--verify', '--strict', '--verbose=2', str(app))

    receipt = {
        'app': str(app),
        'version': args.version,
        'runtime': args.runtime,
        'runtime_manifest_sha256': hashlib.sha256((package / 'runtime-manifest.json').read_bytes()).hexdigest(),
        'min_macos': minimum,
        'frameworks': sorted(p.name for p in frameworks.iterdir()),
        'main_executable_sha256': hashlib.sha256(main_exe.read_bytes()).hexdigest(),
    }
    (out / 'fukami-app-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if args.zip:
        archive = out / f'Fukami-{args.version}.zip'
        archive.unlink(missing_ok=True)
        # No resource forks or extended attributes: they become `._*` files that `unzip` extracts next to the real
        # ones and that break the code-signature seal (Finder merges them silently, other tools do not).
        run('ditto', '-c', '-k', '--norsrc', '--noextattr', '--noqtn', '--keepParent', str(app), str(archive))
        with tempfile.TemporaryDirectory() as check:
            run('unzip', '-q', str(archive), '-d', check)
            run('codesign', '--verify', '--deep', '--strict', str(Path(check) / 'Fukami.app'))
    print(f'Fukami.app {args.version} from {args.runtime}: {app} (macOS {minimum}+, frameworks: {", ".join(receipt["frameworks"])})')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
