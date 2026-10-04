#!/usr/bin/env python3
"""Assemble the Fukami Linux x86-64 / Steam Deck app from a packaged runtime (Gate 5).

Usage (inside the rrv-linux-build:1 container, see tools/linux-build/):
  python3 scripts/package_fukami_linux.py --runtime NAME [--version 0.3.0]
      [--output local/app] [--workload local/gate3/product-live-v1]

Layout (the Linux mirror of Fukami.app/Contents; the runtime's first-launch
hook finds share/ as dirname(/proc/self/exe)/../share):

  Fukami/
    Fukami                          launcher: LD_LIBRARY_PATH=lib, exec bin/Fukami
    Fukami.desktop                  desktop entry (Steam: "Add a Non-Steam Game")
    install.sh                      copies the app to ~/Applications/Fukami, menu entry, adds it to Steam
    bin/Fukami                      runtime/NAME/bin/Fukami
    lib/                            SDL2 + the GS bridge's bundled .so files
    share/fukami-default.ini        rrv.ini with the app defaults
    share/workload/bound-workload.txt
    share/game/.rrv-resource-package.lock
    share/game/.rrv-resource-packages/current/   (byte-identical copy)
    share/fukami.png (--icon), LICENSE.txt, LICENSE-NOTICES.txt

Every library except the SteamOS base set (SYSTEM_LIBS) is bundled. glibc,
libstdc++, the Vulkan loader, X11/Wayland, audio and D-Bus come from the
system: bundling a loader or libc breaks Steam's runtime.

The output holds the compiled game code (owner option A, 2026-09-29): it goes
to an ignored folder and is never committed. No disc data, ELF/IRX or
captures are copied; the script refuses if any appears in the tree.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
# SteamOS (Arch) base libraries: never bundled. Everything else a binary
# needs is copied into lib/ (e.g. Ubuntu's libpcap.so.0.8, which Arch names
# libpcap.so.1). Checked by running the tarball in an archlinux container.
SYSTEM_LIBS = re.compile(
    r'^(ld-linux.*|lib(c|m|dl|pthread|rt|resolv|util|stdc\+\+|gcc_s|atomic)\.so.*|'
    r'libvulkan\.so.*|lib(GL|GLX|GLdispatch|EGL|OpenGL|gbm|drm)\.so.*|'
    r'libX.*|libxcb.*|libwayland.*|libxkbcommon.*|libdecor.*|'
    r'lib(dbus-1|udev|systemd|asound|pulse.*|pipewire.*|fontconfig|freetype|z|curl|expat|ffi)\.so.*)$')
# Loaded with dlopen, so no DT_NEEDED entry names them: PCSX2's Vulkan shader
# cache opens shaderc by versioned name (pcsx2/GS/Renderers/Vulkan/
# VKShaderCache.cpp, GetVersionedFilename("shaderc_shared", 1)). Without it
# GSopen(vulkan) fails with no console message.
DLOPENED = ('/opt/rrv-deps/lib/libshaderc_shared.so.1',)
FORBIDDEN_NAMES = re.compile(r'(?i)(\.(elf|irx|iso|chd|bin|img|gsr|gs|irf)$|^SLUS_|^R5\.ALL$|^SYSTEM\.CNF$)')

sys.path.insert(0, str(ROOT / 'scripts'))
from package_fukami_app import default_ini  # noqa: E402  (same ini defaults as the Mac app)

LAUNCHER = r'''#!/bin/sh
# Fukami launcher: use the bundled libraries, then start the game.
# Arguments pass through (e.g. a .chd path on first launch).
HERE=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/bin/Fukami" "$@"
'''

DESKTOP = '''[Desktop Entry]
Type=Application
Name=Fukami
Comment=Native port runtime for Ridge Racer V (bring your own disc)
Exec=Fukami
Icon=fukami
Terminal=false
Categories=Game;
'''


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def ldd(path: Path, lib_dir: Path) -> dict[str, str]:
    env = dict(os.environ, LD_LIBRARY_PATH=f'{lib_dir}:{os.environ.get("LD_LIBRARY_PATH", "")}')
    out = subprocess.run(['ldd', str(path)], check=True, capture_output=True, text=True, env=env).stdout
    deps = {}
    for line in out.splitlines():
        match = re.match(r'\s*(\S+) => (\S+)', line)
        if match:
            if match.group(2) == 'not':
                sys.exit(f'{path}: unresolved dependency {match.group(1)}')
            deps[match.group(1)] = match.group(2)
    return deps


def needed(path: Path) -> list[str]:
    return run('patchelf', '--print-needed', str(path)).split()


def bundle_closure(starts: list[Path], lib_dir: Path) -> list[str]:
    """Copy every non-system library the starts need into lib_dir under its
    soname, following DT_NEEDED only through bundled copies (ldd's full
    transitive list would drag in the build box's copies of what system
    libraries such as libcurl need), and set each copy's RUNPATH to $ORIGIN."""
    copied: dict[str, Path] = {}
    queue = list(starts)
    while queue:
        loader = queue.pop()
        resolved = ldd(loader, lib_dir)
        for soname in needed(loader):
            if soname in copied or SYSTEM_LIBS.match(soname):
                continue
            source = resolved.get(soname)
            if source is None:
                sys.exit(f'{loader}: cannot resolve {soname}')
            target = lib_dir / soname
            shutil.copy2(Path(source).resolve(), target)
            os.chmod(target, 0o755)
            run('patchelf', '--set-rpath', '$ORIGIN', str(target))
            copied[soname] = target
            queue.append(target)
    return sorted(copied)


def notices(libs: list[str]) -> str:
    lines = [
        'Fukami - a static-recompilation port runtime for Ridge Racer V (USA).',
        'Licence: GNU General Public License v3.0.',
        'You must supply your own copy of the game; no game data is included.',
        'Not affiliated with or endorsed by Bandai Namco.',
        '',
        'Third-party components:',
        '- PCSX2 2.8.2 (GS renderer, SPU2 core, libchdr/lzma): GPL-3.0 and component licences.',
        '- SDL 2.32.10: zlib licence. SDL 3.4.12: zlib licence.',
        '- libchdr: BSD-3-Clause. zstd: BSD. lzma SDK: public domain.',
        '- shaderc/glslang/SPIRV-Tools: Apache-2.0 / BSD. plutovg/plutosvg: MIT. rapidyaml: MIT.',
        '- libpng: libpng licence. libjpeg-turbo: IJG/BSD. libwebp: BSD. lz4: BSD.',
    ]
    lines += [f'- {name}' for name in libs]
    return '\n'.join(lines) + '\n'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--runtime', required=True, help='package name under runtime/')
    parser.add_argument('--version', default=(ROOT / 'VERSION').read_text().strip() if (ROOT / 'VERSION').is_file() else '0.3.0',
                        help='default: the VERSION file')
    parser.add_argument('--output', type=Path, default=ROOT / 'local/app')
    parser.add_argument('--workload', type=Path,
                        default=(ROOT / 'local/gate3/product-live-v1' if (ROOT / 'local/gate3/product-live-v1/bound-workload.txt').is_file()
                                 else ROOT / 'config/product'),
                        help='directory holding bound-workload.txt (default: your local one, else config/product)')
    parser.add_argument('--icon', type=Path, default=ROOT / 'tools/fukami-app/fukami.png',
                        help='PNG drawn on macOS by tools/fukami-app/make_icon.py (it needs the '
                             'macOS system fonts): make_icon.py DIR.iconset, then copy icon_512x512@2x.png')
    args = parser.parse_args()
    if sys.platform != 'linux':
        sys.exit('run inside the rrv-linux-build container (tools/linux-build/)')

    package = ROOT / 'runtime' / args.runtime
    subprocess.run([sys.executable, str(ROOT / 'scripts/package_gate4_product.py'), '--verify',
                    '--name', args.runtime], check=True)
    exe = package / 'bin/Fukami'
    if not exe.is_file():  # a runtime packaged before the executable was renamed
        exe = package / 'bin/rrv-gate3-candidate'
    bound = args.workload / 'bound-workload.txt'
    if 'storage persistent_user_card' not in bound.read_text().splitlines():
        sys.exit(f'{bound} is not a product workload')

    out = args.output.resolve() / f'{args.version}-linux'
    app = out / 'Fukami'
    if app.exists():
        shutil.rmtree(app)
    bin_dir, lib_dir, share = app / 'bin', app / 'lib', app / 'share'
    for folder in (bin_dir, lib_dir, share / 'workload', share / 'game'):
        folder.mkdir(parents=True)

    main_exe = bin_dir / 'Fukami'
    shutil.copy2(exe, main_exe)
    run('patchelf', '--set-rpath', '$ORIGIN/../lib', str(main_exe))
    (share / 'game/.rrv-resource-package.lock').touch()
    shutil.copytree(package / '.rrv-resource-packages', share / 'game/.rrv-resource-packages',
                    symlinks=False)
    bridge = share / 'game/.rrv-resource-packages/current/librrv-pcsx2-gs-bridge.so'
    # The bridge is hash-checked by the runtime, so it is never patched; the
    # launcher's LD_LIBRARY_PATH finds its dependencies in lib/.
    for extra in map(Path, DLOPENED):
        shutil.copy2(extra.resolve(), lib_dir / extra.name)
        os.chmod(lib_dir / extra.name, 0o755)
        run('patchelf', '--set-rpath', '$ORIGIN', str(lib_dir / extra.name))
    libs = bundle_closure([main_exe, bridge, *(lib_dir / Path(e).name for e in DLOPENED)], lib_dir)
    libs = sorted(set(libs) | {Path(e).name for e in DLOPENED})
    for pkg_lib in (package / 'lib').glob('*.so*'):
        if pkg_lib.name not in libs:
            shutil.copy2(pkg_lib, lib_dir / pkg_lib.name)
            libs.append(pkg_lib.name)
    shutil.copy2(bound, share / 'workload/bound-workload.txt')
    # The package defaults. The launcher copies this file to the user's
    # fukami.ini on first launch, and a key that file does not have (written by
    # an older version) takes its value from here (settings::withDefaults).
    # Steam Deck screen is 1280x800: 2x internal (1280x896 for a 640x448
    # field pair) already fills it; the Mac default of 4x only costs GPU time.
    ini = re.sub(r'(?m)^scale = .*$', 'scale = 2', default_ini())
    # split_gs and pacer_spin are the same on every platform now (rrv.ini: split_gs = true, pacer_spin = 0).
    (share / 'fukami-default.ini').write_text(ini)
    (share / 'LICENSE-NOTICES.txt').write_text(notices(sorted(libs)))
    shutil.copy2(ROOT / 'LICENSE', share / 'LICENSE.txt')
    shutil.copy2(args.icon, share / 'fukami.png')
    (app / 'Fukami').write_text(LAUNCHER)
    os.chmod(app / 'Fukami', 0o755)
    (app / 'Fukami.desktop').write_text(DESKTOP)
    shutil.copy2(ROOT / 'tools/linux-build/install.sh', app / 'install.sh')
    os.chmod(app / 'install.sh', 0o755)

    for path in app.rglob('*'):
        if FORBIDDEN_NAMES.search(path.name):
            sys.exit(f'refusing to ship {path}')

    # Every bundled library must resolve with the system + lib/ only.
    for target in [main_exe, bridge, *lib_dir.iterdir()]:
        ldd(target, lib_dir)

    receipt = {
        'app': str(app),
        'version': args.version,
        'platform': 'linux-x86_64',
        'runtime': args.runtime,
        'runtime_manifest_sha256': hashlib.sha256((package / 'runtime-manifest.json').read_bytes()).hexdigest(),
        'glibc_built_against': os.confstr('CS_GNU_LIBC_VERSION'),
        'libs': sorted(p.name for p in lib_dir.iterdir()),
        'main_executable_sha256': hashlib.sha256(main_exe.read_bytes()).hexdigest(),
    }
    (out / 'fukami-linux-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    archive = out / f'Fukami-{args.version}-linux-x86_64.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(app, arcname='Fukami')
    print(f'Fukami {args.version} (linux-x86_64) from {args.runtime}: {archive}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
