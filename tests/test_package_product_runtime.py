#!/usr/bin/env python3
"""Tamper coverage for the ADR-0006 package export receipt."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import contextlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('package', ROOT / 'scripts/package_product_runtime.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class PackageReceiptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.target = self.root / 'runtime'
        self.prefix = self.root / 'sdl'
        (self.target / 'bin').mkdir(parents=True)
        (self.target / '.rrv-resource-package.lock').touch()
        (self.target / '.rrv-resource-packages/current/resources').mkdir(parents=True)
        (self.prefix / 'lib').mkdir(parents=True)
        (self.target / 'bin/rrv-product').write_bytes(b'product')
        (self.target / 'bin/product-identity-receipt.json').write_bytes(b'identity')
        (self.target / '.rrv-resource-packages/current/identity').write_text('1\ncomposition\n')
        (self.target / '.rrv-resource-packages/current/bridge.dylib').write_bytes(b'bridge')
        (self.target / '.rrv-resource-packages/current/resources/Metal23.metallib').write_bytes(b'metal')
        self.library = self.prefix / 'lib/libSDL2.dylib'
        self.library.write_bytes(b'sdl')
        self.identity = {'sdl': {'library': {'sha256': digest(self.library)}}}
        receipt = {'schema_version': 3, 'kind': 'rrv-resource-package-export',
                   'product_identity_sha256': digest(self.target / 'bin/product-identity-receipt.json'),
                   'resource_package_identity': '1\ncomposition\n',
                   'resource_package_inventory': package.package_inventory(
                       self.target / '.rrv-resource-packages/current'),
                   'sdl_relative_path': 'lib/libSDL2.dylib',
                   'sdl_load_path': '@loader_path/../../sdl/lib/libSDL2.dylib',
                   'files': package.runtime_inventory(self.target)}
        (self.target / 'runtime-manifest.json').write_text(json.dumps(receipt))

    def tearDown(self):
        self.temp.cleanup()

    def verify(self):
        linkage = '\t@loader_path/../../sdl/lib/libSDL2.dylib (compatibility version 0, current version 0)\n'
        with mock.patch.object(package, 'verify_identity', return_value=self.identity), \
             mock.patch.object(package, 'sdl_library', return_value=self.library), \
             mock.patch.object(package, 'run', return_value='rrv-product:\n' + linkage):
            package.verify_runtime(self.target, self.prefix)

    def test_export_receipt_binds_identity_package_and_layout(self):
        self.verify()

    def test_payload_tamper_fails(self):
        (self.target / '.rrv-resource-packages/current/bridge.dylib').write_bytes(b'tampered')
        with self.assertRaisesRegex(SystemExit, 'typed inventory changed'):
            self.verify()

    def test_package_identity_tamper_fails(self):
        (self.target / '.rrv-resource-packages/current/identity').write_text('bad\n')
        with self.assertRaisesRegex(SystemExit, 'typed inventory changed'):
            self.verify()

    def test_sdl_identity_mismatch_fails(self):
        self.library.write_bytes(b'other')
        with self.assertRaisesRegex(SystemExit, 'SDL library differs'):
            self.verify()

    def test_package_extra_directory_fails(self):
        (self.target / '.rrv-resource-packages/current/unexpected').mkdir()
        with self.assertRaisesRegex(SystemExit, 'typed inventory changed'):
            self.verify()

    def test_missing_runtime_lock_fails(self):
        (self.target / '.rrv-resource-package.lock').unlink()
        with self.assertRaisesRegex(SystemExit, 'mandatory .rrv-resource-package.lock'):
            self.verify()

    def test_runtime_executable_symlink_is_rejected(self):
        executable = self.target / 'bin/rrv-product'
        executable.unlink()
        os.symlink(self.target / '.rrv-resource-packages/current/bridge.dylib', executable)
        with self.assertRaisesRegex(SystemExit, 'unsafe node'):
            self.verify()

    def test_runtime_executable_hardlink_is_rejected(self):
        executable = self.target / 'bin/rrv-product'
        os.link(executable, self.target / 'bin/rrv-product-copy')
        with self.assertRaisesRegex(SystemExit, 'unsafe node'):
            self.verify()

    def test_runtime_extra_directory_is_rejected(self):
        (self.target / 'unexpected').mkdir()
        with self.assertRaisesRegex(SystemExit, 'typed inventory changed'):
            self.verify()

    def test_package_symlink_fails(self):
        bridge = self.target / '.rrv-resource-packages/current/bridge.dylib'
        bridge.unlink()
        os.symlink(self.target / 'bin/rrv-product', bridge)
        with self.assertRaisesRegex(SystemExit, 'symlink'):
            package.package_inventory(self.target / '.rrv-resource-packages/current')

    def test_package_hardlink_fails(self):
        bridge = self.target / '.rrv-resource-packages/current/bridge.dylib'
        os.link(bridge, self.target / '.rrv-resource-packages/current/bridge-copy.dylib')
        with self.assertRaisesRegex(SystemExit, 'hard-linked'):
            package.package_inventory(self.target / '.rrv-resource-packages/current')

    def test_package_parent_symlink_rejects_outside_without_mutation(self):
        package_parent = self.target / '.rrv-resource-packages'
        outside = self.root / 'outside-package-parent'
        package_parent.rename(outside)
        sentinel = outside / 'sentinel'
        sentinel.write_bytes(b'outside remains untouched')
        os.symlink(outside, package_parent)
        with self.assertRaisesRegex(SystemExit, 'unsafe node'):
            self.verify()
        self.assertEqual(sentinel.read_bytes(), b'outside remains untouched')

    def test_source_package_symlink_is_rejected_before_export(self):
        source = self.root / 'source'
        source_package = source / '.rrv-resource-packages/current'
        source_package.mkdir(parents=True)
        (source_package / 'identity').write_text('1\ncomposition\n')
        bridge = source_package / 'bridge.dylib'
        bridge.write_bytes(b'bridge')
        identity = {'bridge': {'resource_package': {
            'published_inventory': package.package_inventory(source_package)}}}
        bridge.unlink()
        os.symlink(self.target / 'bin/rrv-product', bridge)
        with self.assertRaisesRegex(SystemExit, 'symlink'):
            package.package_from_identity(identity, source)

    def test_source_package_parent_symlink_is_rejected_before_copy(self):
        source = self.root / 'source-parent-link'
        parent = source / '.rrv-resource-packages'
        current = parent / 'current'
        current.mkdir(parents=True)
        (current / 'identity').write_text('1\\ncomposition\\n')
        (current / 'bridge.dylib').write_bytes(b'bridge')
        outside = self.root / 'outside-source-parent'
        parent.rename(outside)
        sentinel = outside / 'sentinel'
        sentinel.write_bytes(b'unchanged')
        os.symlink(outside, parent)
        identity = {'bridge': {'resource_package': {'published_inventory': []}}}
        with self.assertRaisesRegex(SystemExit, 'symlink'):
            package.package_from_identity(identity, source)
        self.assertEqual(sentinel.read_bytes(), b'unchanged')

    def test_substituted_source_executable_is_rejected_before_copy(self):
        source = self.root / 'source-build'
        (source / 'bin').mkdir(parents=True)
        (source / '.rrv-resource-package.lock').touch()
        (source / 'bin/rrv-product').write_bytes(b'substituted')
        (source / 'bin/product-identity-receipt.json').write_text('{}')
        staged = self.root / 'staged'
        (staged / 'bin').mkdir(parents=True)
        expected = {'product': {'binary': {'size': len(b'original'), 'sha256': digest(self.target / 'bin/rrv-product')}}}
        with mock.patch.object(package, 'lease', return_value=contextlib.nullcontext()), \
             mock.patch.object(package, 'verify_identity', return_value=expected):
            with self.assertRaisesRegex(SystemExit, 'selected product executable differs'):
                package.copy_source_export_inputs(source, staged)


if __name__ == '__main__':
    unittest.main()
