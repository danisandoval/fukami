#!/usr/bin/env python3
"""Export one source-derived ADR-0006 product package into a new runtime root."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile

from prepare_sdl import publish_directory
from resource_package_fs import PackageFilesystemError, lease, typed_inventory

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 3


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args: object) -> str:
    return subprocess.check_output([str(arg) for arg in args], text=True)


def runtime_inventory(directory: Path) -> list[dict[str, object]]:
    """Whole-export typed inventory, excluding only the receipt itself."""
    try:
        return [entry for entry in typed_inventory(directory.absolute())
                if entry['path'] != 'runtime-manifest.json']
    except PackageFilesystemError as exc:
        raise SystemExit(f'runtime export contains unsafe node: {exc}') from exc


def package_inventory(directory: Path) -> list[dict[str, object]]:
    try:
        return typed_inventory(directory.absolute())
    except PackageFilesystemError as exc:
        raise SystemExit(str(exc)) from exc


def verify_lock(lock: Path) -> None:
    try:
        info = os.lstat(lock)
    except FileNotFoundError:
        raise SystemExit('runtime package omits mandatory .rrv-resource-package.lock') from None
    if not stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode) or info.st_nlink != 1:
        raise SystemExit('runtime resource package lock is not a safe regular file')


def verify_identity(receipt: Path) -> dict:
    subprocess.run([sys.executable, str(ROOT / 'scripts/product_identity_receipt.py'),
                    'verify', '--receipt', str(receipt)], check=True)
    return json.loads(receipt.read_text())


def require_identity_sdl(identity: dict, library: Path, context: str) -> None:
    expected = identity.get('sdl', {}).get('library', {}).get('sha256')
    if not isinstance(expected, str) or digest(library) != expected:
        raise SystemExit(f'SDL library differs from product identity receipt for {context}')


def copy_source_export_inputs(source_root: Path, staged: Path) -> tuple[dict, Path, dict[str, object]]:
    """Read and copy the source package while retaining its canonical reader lease."""
    try:
        with lease(source_root / '.rrv-resource-package.lock', False):
            executable = source_root / 'bin' / 'rrv-product'
            identity = source_root / 'bin' / 'product-identity-receipt.json'
            for required in (executable, identity):
                if not required.is_file():
                    raise SystemExit(f'missing source build input: {required}')
            identity_data = verify_identity(identity)
            expected_binary = identity_data.get('product', {}).get('binary', {})
            actual_binary = executable.stat()
            if (not isinstance(expected_binary, dict) or
                    expected_binary.get('size') != actual_binary.st_size or
                    expected_binary.get('sha256') != digest(executable)):
                raise SystemExit('selected product executable differs from verified product identity receipt')
            source_package = package_from_identity(identity_data, source_root)
            staged_executable = staged / 'bin' / 'rrv-product'
            shutil.copy2(executable, staged_executable)
            shutil.copy2(identity, staged / 'bin' / 'product-identity-receipt.json')
            shutil.copytree(source_package, staged / '.rrv-resource-packages' / 'current')
            return identity_data, identity, {'identity': (source_package / 'identity').read_text(),
                                             'inventory': package_inventory(source_package)}
    except PackageFilesystemError as exc:
        raise SystemExit(f'source resource package lease failed: {exc}') from exc


def package_from_identity(identity: dict, source_root: Path) -> Path:
    package = identity.get('bridge', {}).get('resource_package', {})
    if not isinstance(package, dict):
        raise SystemExit('product identity receipt omits source-derived resource package')
    source_package = source_root / '.rrv-resource-packages' / 'current'
    if not source_package.is_dir():
        raise SystemExit(f'source-derived package is unavailable: {source_package}')
    expected = package.get('published_inventory')
    if not isinstance(expected, list):
        raise SystemExit('product identity receipt omits published package inventory')
    if package_inventory(source_package) != expected:
        raise SystemExit('source-derived package differs from product identity receipt')
    return source_package


def sdl_library(prefix: Path) -> Path:
    subprocess.run([sys.executable, str(ROOT / 'scripts/prepare_sdl.py'),
                    '--verify', '--prefix', str(prefix)], check=True)
    manifest = json.loads((prefix / 'manifest.json').read_text())
    library = prefix / manifest['library']
    if not library.is_file():
        raise SystemExit(f'SDL library is unavailable: {library}')
    return library


def verify_runtime(target: Path, prefix: Path, *, frozen_source: bool = False) -> None:
    receipt_path = target / 'runtime-manifest.json'
    receipt = json.loads(receipt_path.read_text())
    if receipt.get('schema_version') != SCHEMA or receipt.get('kind') != 'rrv-resource-package-export':
        raise SystemExit('unsupported ADR-0006 runtime package receipt schema')
    verify_lock(target / '.rrv-resource-package.lock')
    expected = receipt.get('files')
    actual = runtime_inventory(target)
    if not isinstance(expected, list) or actual != expected:
        raise SystemExit('runtime export typed inventory changed')
    identity = target / 'bin' / 'product-identity-receipt.json'
    if digest(identity) != receipt.get('product_identity_sha256'):
        raise SystemExit('runtime product identity receipt is missing or changed')
    # A packaged executable is immutable. Launch verification checks its exported
    # bytes even when development has moved the source checkout past its receipt.
    # Full qualification still uses verify_identity() against the live source.
    identity_data = json.loads(identity.read_text()) if frozen_source else verify_identity(identity)
    package = target / '.rrv-resource-packages' / 'current'
    package_identity = package / 'identity'
    expected_identity = receipt.get('resource_package_identity')
    if not package_identity.is_file() or package_identity.read_text() != expected_identity:
        raise SystemExit('runtime package identity differs from export receipt')
    if package_inventory(package) != receipt.get('resource_package_inventory'):
        raise SystemExit('runtime resource package typed inventory changed')
    require_identity_sdl(identity_data, sdl_library(prefix), 'runtime verification')
    executable = target / 'bin' / 'rrv-product'
    linkage = run('otool', '-L', executable)
    expected_load = '@loader_path/' + os.path.relpath(prefix / receipt['sdl_relative_path'], executable.parent)
    actual_loads = [line.strip().split(' (', 1)[0] for line in linkage.splitlines()[1:]
                    if 'libSDL2' in line]
    if receipt.get('sdl_load_path') != expected_load or actual_loads != [expected_load]:
        raise SystemExit('runtime SDL load path does not resolve to the verified prefix')


def verify_historical_runtime(target: Path) -> None:
    """Keep the recorded pre-ADR bundle launchable without qualifying it."""
    receipt_path = target / 'runtime-manifest.json'
    receipt = json.loads(receipt_path.read_text())
    files = receipt.get('files')
    if receipt.get('schema_version') != 1 or not isinstance(files, dict):
        raise SystemExit('unsupported historical runtime package receipt schema')
    for relative, expected in files.items():
        if not isinstance(relative, str) or not isinstance(expected, str):
            raise SystemExit('historical runtime receipt has malformed file inventory')
        candidate = target / relative
        if candidate.parent != target and target not in candidate.parents:
            raise SystemExit('historical runtime receipt escapes its bundle')
        if not candidate.is_file() or digest(candidate) != expected:
            raise SystemExit('historical runtime package file inventory changed')
    if not (target / 'rrv-product').is_file():
        raise SystemExit('historical runtime product executable is unavailable')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--source-build-root', type=Path)
    parser.add_argument('--sdl-prefix', type=Path,
                        default=ROOT / 'runtime-deps/sdl/2.32.10/macos-arm64')
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--verify-frozen', action='store_true')
    parser.add_argument('--verify-historical', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]*', args.name):
        parser.error('name must contain lowercase letters, digits and hyphens')
    target = ROOT / 'runtime' / args.name
    prefix = args.sdl_prefix.resolve()
    if sum((args.verify, args.verify_frozen, args.verify_historical)) > 1:
        parser.error('verification modes are mutually exclusive')
    if args.verify or args.verify_frozen:
        verify_runtime(target, prefix, frozen_source=args.verify_frozen)
        scope = 'frozen runtime export' if args.verify_frozen else 'ADR-0006 runtime export'
        print(f'verified {scope}: {target}')
        return
    if args.verify_historical:
        verify_historical_runtime(target)
        print(f'verified historical runtime (not ADR-0006 qualified): {target}')
        return
    if args.source_build_root is None:
        parser.error('--source-build-root is required when exporting')
    source_root = args.source_build_root.resolve()
    library = sdl_library(prefix)
    if target.exists():
        raise SystemExit(f'refusing to overwrite existing runtime: {target}')
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.rrv-resource-export-', dir=target.parent) as staging:
        staged = Path(staging) / 'runtime-root'
        (staged / 'bin').mkdir(parents=True)
        lock = staged / '.rrv-resource-package.lock'
        lock_fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.close(lock_fd)
        verify_lock(lock)
        identity_data, identity, package_data = copy_source_export_inputs(source_root, staged)
        require_identity_sdl(identity_data, library, 'runtime export')
        staged_executable = staged / 'bin' / 'rrv-product'
        staged_executable.chmod(staged_executable.stat().st_mode | 0o200)
        old_loads = [line.strip().split(' (', 1)[0] for line in run('otool', '-L', staged_executable).splitlines()[1:]
                     if 'libSDL2' in line]
        if len(old_loads) != 1:
            raise SystemExit('expected exactly one SDL2 load command')
        # The staged root is one directory deeper than the published bundle.
        # Encode the final runtime/<name>/bin relation, never the staging one.
        new_load = '@loader_path/' + os.path.relpath(library, target / 'bin')
        subprocess.run(['install_name_tool', '-change', old_loads[0], new_load, str(staged_executable)], check=True)
        subprocess.run(['codesign', '--force', '--sign', '-', str(staged_executable)], check=True)
        subprocess.run(['codesign', '--verify', '--strict', str(staged_executable)], check=True)
        receipt = {'schema_version': SCHEMA, 'kind': 'rrv-resource-package-export',
                   'source_build_root': str(source_root),
                   'product_identity_sha256': digest(identity),
                   'resource_package_identity': package_data['identity'],
                   'resource_package_inventory': package_data['inventory'],
                   'sdl_relative_path': str(library.relative_to(prefix)),
                   'sdl_load_path': new_load,
                   'note': 'A new bundle-relative source-derived package export; existing runtime bundles were not refreshed.',
                   'files': {}}
        (staged / 'runtime-manifest.json').write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')
        receipt['files'] = runtime_inventory(staged)
        (staged / 'runtime-manifest.json').write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')
        publish_directory(staged, target)
    print(f'exported ADR-0006 runtime: {target}')


if __name__ == '__main__':
    main()
