#!/usr/bin/env python3
"""Tamper tests for the Gate-0 source-bound product identity receipt."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
# The receipt lazily imports sibling scripts (resource_package_fs).
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('identity', ROOT / 'scripts/product_identity_receipt.py')
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)


def write(path: Path, value: bytes = b'x') -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value)
    return path


def git_repo(path: Path) -> Path:
    path.mkdir()
    subprocess.run(['git', 'init', '-q', str(path)], check=True)
    subprocess.run(['git', '-C', str(path), 'config', 'user.email', 'gate@example.invalid'], check=True)
    subprocess.run(['git', '-C', str(path), 'config', 'user.name', 'Gate'], check=True)
    write(path / 'tracked', b'base')
    subprocess.run(['git', '-C', str(path), 'add', 'tracked'], check=True)
    subprocess.run(['git', '-C', str(path), 'commit', '-qm', 'base'], check=True)
    return path


class ReceiptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = git_repo(self.root / 'rrv')
        self.jit = git_repo(self.root / 'sse2neon')
        self.build = self.root / 'build'
        self.build.mkdir()
        self.binary = write(self.build / 'rrv-product', b'binary')
        self.generated = self.root / 'generated'
        write(self.generated / 'register_functions.cpp', b'generated')
        self.generation = write(self.root / 'generation.json', json.dumps(
            {'generated': {'files': [{'path': 'register_functions.cpp'}]}}).encode())
        self.bridge_library = write(self.root / 'bridge.dylib', b'bridge')
        self.resources = self.root / 'resources'
        write(self.resources / 'metal.metallib', b'metal')
        self.metal_source = write(self.root / 'metal-source.json', b'{}')
        self.metal_verifier = write(self.root / 'metal-verifier.toml', b'')
        self.bridge = write(self.root / 'bridge.json', json.dumps({
            'library': {'path': str(self.bridge_library)},
            'runtime_resources': {'directory': str(self.resources)},
            'metal_resources': {'source_manifest': str(self.metal_source),
                                'verifier_manifest': str(self.metal_verifier)},
        }).encode())
        self.bridge_verifier = write(self.root / 'bridge-verifier.py', b'#!/usr/bin/env python3\n')
        self.package_root = self.build / '.rrv-resource-packages/current'
        write(self.package_root / 'bridge.dylib', b'bridge')
        write(self.package_root / 'resources/metal.metallib', b'metal')
        write(self.package_root / 'identity', b'1\ncomposition\n')
        self.package_spec = write(self.build / 'CMakeFiles/rrv-resource-package/spec.json', json.dumps({
            'schema': 'rrv-resource-package-spec-v1', 'version': 1,
            'composition': 'composition', 'bridge': 'bridge.dylib',
            'inventory': [], 'sources': [], 'provenance': {},
        }).encode())
        self.sdl_prefix = self.root / 'sdl'
        self.sdl_library = write(self.sdl_prefix / 'lib/libSDL2.dylib', b'sdl')
        self.sdl_manifest = write(self.sdl_prefix / 'manifest.json', json.dumps({'library': 'lib/libSDL2.dylib'}).encode())
        self.abi = write(self.root / 'bridge.h', b'enum { RRV_PCSX2_GS_BRIDGE_ABI_VERSION = 5 };')
        self.overlays = []
        for name in ('product-host', 'm2-causal', 'spr-pending-chain', 'm2p-pad-observer',
                     'pad-pressure', 'm2p-game001-fail-closed', 'product-host-final',
                     'callback-stack-main-reservation', 'iop-heap', 'rpc-memory-safety', 'gs-control'):
            self.overlays.append(f'{name}=inactive' if name in ('m2-causal', 'm2p-pad-observer', 'm2p-game001-fail-closed', 'iop-heap', 'rpc-memory-safety', 'gs-control')
                                 else f'{name}=active:{write(self.root / (name + ".json"), b"{}") }')
        self.scripts = [(name, write(self.root / (name + '.py')))
                        for name in ('product-host', 'm2-causal', 'spr-pending-chain', 'm2p-pad-observer',
                                     'pad-pressure', 'm2p-game001-fail-closed', 'product-host-final',
                                     'callback-stack-main-reservation', 'iop-heap', 'rpc-memory-safety', 'gs-control')]
        self.metadata_files = [write(self.build / name, b'config')
                               for name in ('CMakeCache.txt', 'build.ninja', 'compile_commands.json', 'product-no-raylib.json')]
        (self.build / 'CMakeCache.txt').write_text(
            'RRV_IOP_HEAP_ISOLATION:BOOL=OFF\nRRV_RPC_MEMORY_SAFETY:BOOL=OFF\n')
        self.source_input = write(self.root / 'source.cpp', b'int main() {}')
        self.header_input = write(self.root / 'source.h', b'#pragma once')
        self.metadata = {
            'cmake_cache': identity.file_entry(self.build / 'CMakeCache.txt'),
            'ninja_file': identity.file_entry(self.build / 'build.ninja'),
            'compile_commands': identity.file_entry(self.build / 'compile_commands.json'),
            'target_commands_sha256': 'a' * 64, 'target_commands_count': 1,
            'source_inputs': [identity.file_entry(self.source_input)],
            'source_inputs_sha256': identity.inventory_digest([identity.file_entry(self.source_input)]),
            'header_inputs': [identity.file_entry(self.header_input)],
            'header_inputs_sha256': identity.inventory_digest([identity.file_entry(self.header_input)]),
            'post_link_audit': identity.file_entry(self.build / 'product-no-raylib.json'),
        }
        self.receipt = self.build / 'product-identity-receipt.json'

    def tearDown(self):
        self.temp.cleanup()

    def create(self):
        argv = ['create', '--source-root', str(self.source), '--build-dir', str(self.build),
                '--binary', str(self.binary), '--generation-manifest', str(self.generation),
                '--generated-dir', str(self.generated), '--bridge-manifest', str(self.bridge),
                '--bridge-manifest-verifier', str(self.bridge_verifier), '--bridge-manifest-verifier-text', str(self.bridge_verifier),
                '--resource-package-spec', str(self.package_spec), '--resource-package-root', str(self.package_root),
                '--sdl-manifest', str(self.sdl_manifest), '--architecture', 'arm64', '--jit-source', str(self.jit),
                '--abi-header', str(self.abi), '--output', str(self.receipt)]
        for value in self.overlays:
            argv += ['--overlay-stage', value]
        for name, value in self.scripts:
            argv += ['--overlay-script', f'{name}={value}']
        with mock.patch.object(identity, 'metadata_from_ninja', return_value=self.metadata), \
             mock.patch.object(sys, 'argv', ['receipt'] + argv):
            self.assertEqual(identity.main(), 0)

    def create_x86(self):
        argv = ['create', '--source-root', str(self.source), '--build-dir', str(self.build),
                '--binary', str(self.binary), '--generation-manifest', str(self.generation),
                '--generated-dir', str(self.generated), '--bridge-manifest', str(self.bridge),
                '--bridge-manifest-verifier', str(self.bridge_verifier), '--bridge-manifest-verifier-text', str(self.bridge_verifier),
                '--resource-package-spec', str(self.package_spec), '--resource-package-root', str(self.package_root),
                '--sdl-manifest', str(self.sdl_manifest), '--architecture', 'x86_64',
                '--abi-header', str(self.abi), '--output', str(self.receipt)]
        for value in self.overlays:
            argv += ['--overlay-stage', value]
        for name, value in self.scripts:
            argv += ['--overlay-script', f'{name}={value}']
        with mock.patch.object(identity, 'metadata_from_ninja', return_value=self.metadata), \
             mock.patch.object(sys, 'argv', ['receipt'] + argv):
            self.assertEqual(identity.main(), 0)

    def verify(self):
        with mock.patch.object(identity, 'metadata_from_ninja', return_value=self.metadata), \
             mock.patch.object(sys, 'argv', ['receipt', 'verify', '--receipt', str(self.receipt)]):
            return identity.main()

    def test_create_and_verify(self):
        self.create()
        self.assertEqual(self.verify(), 0)

    def test_legacy_overlay_sequence_remains_verifiable(self):
        self.create()
        (self.build / 'CMakeCache.txt').write_text('legacy cache without IOP option\n')
        self.metadata['cmake_cache'] = identity.file_entry(self.build / 'CMakeCache.txt')
        receipt = json.loads(self.receipt.read_text())
        receipt['build']['cmake_cache'] = self.metadata['cmake_cache']
        receipt['overlays'] = [stage for stage in receipt['overlays'] if stage['name'] != 'iop-heap']
        receipt['overlays'] = [stage for stage in receipt['overlays'] if stage['name'] != 'rpc-memory-safety']
        receipt['overlay_scripts'] = [stage for stage in receipt['overlay_scripts']
                                      if stage['name'] not in ('iop-heap', 'rpc-memory-safety')]
        self.receipt.write_text(json.dumps(receipt))
        self.assertEqual(self.verify(), 0)

    def test_iop_era_overlay_sequence_remains_verifiable(self):
        self.create()
        (self.build / 'CMakeCache.txt').write_text('RRV_IOP_HEAP_ISOLATION:BOOL=OFF\n')
        receipt = json.loads(self.receipt.read_text())
        receipt['build']['cmake_cache'] = identity.file_entry(self.build / 'CMakeCache.txt')
        receipt['overlays'] = [stage for stage in receipt['overlays']
                               if stage['name'] != 'rpc-memory-safety']
        receipt['overlay_scripts'] = [stage for stage in receipt['overlay_scripts']
                                      if stage['name'] != 'rpc-memory-safety']
        self.receipt.write_text(json.dumps(receipt))
        self.assertEqual(self.verify(), 0)

    def test_current_cache_rejects_deleted_rpc_stage(self):
        self.create()
        receipt = json.loads(self.receipt.read_text())
        receipt['overlays'] = [stage for stage in receipt['overlays']
                               if stage['name'] != 'rpc-memory-safety']
        receipt['overlay_scripts'] = [stage for stage in receipt['overlay_scripts']
                                      if stage['name'] != 'rpc-memory-safety']
        self.receipt.write_text(json.dumps(receipt))
        self.assertEqual(self.verify(), 1)

    def test_rpc_cache_on_requires_active_rpc_stage(self):
        (self.build / 'CMakeCache.txt').write_text(
            'RRV_IOP_HEAP_ISOLATION:BOOL=ON\nRRV_RPC_MEMORY_SAFETY:BOOL=ON\n')
        self.metadata['cmake_cache'] = identity.file_entry(self.build / 'CMakeCache.txt')
        self.overlays[8] = f'iop-heap=active:{write(self.root / "iop-heap.json", b"{}")}'
        with self.assertRaises(AssertionError):
            self.create()
        self.overlays[9] = f'rpc-memory-safety=active:{write(self.root / "rpc-memory-safety.json", b"{}")}'
        self.create()
        self.assertEqual(self.verify(), 0)

    def test_rpc_stage_requires_iop_heap(self):
        stages = [{'name': 'callback-stack-main-reservation', 'active': True},
                  {'name': 'iop-heap', 'active': False},
                  {'name': 'rpc-memory-safety', 'active': True}]
        with self.assertRaises(RuntimeError):
            identity.check_overlay_pairing(stages, [{'name': stage['name']} for stage in stages])

    def test_current_cache_rejects_deleted_iop_stage(self):
        self.create()
        receipt = json.loads(self.receipt.read_text())
        receipt['overlays'] = [stage for stage in receipt['overlays'] if stage['name'] != 'iop-heap']
        receipt['overlay_scripts'] = [stage for stage in receipt['overlay_scripts']
                                      if stage['name'] != 'iop-heap']
        self.receipt.write_text(json.dumps(receipt))
        self.assertEqual(self.verify(), 1)

    def test_cache_on_requires_active_iop_stage(self):
        (self.build / 'CMakeCache.txt').write_text(
            'RRV_IOP_HEAP_ISOLATION:BOOL=ON\nRRV_RPC_MEMORY_SAFETY:BOOL=OFF\n')
        self.metadata['cmake_cache'] = identity.file_entry(self.build / 'CMakeCache.txt')
        with self.assertRaises(AssertionError):
            self.create()
        self.overlays[8] = f'iop-heap=active:{write(self.root / "iop-heap.json", b"{}")}'
        self.create()
        self.assertEqual(self.verify(), 0)
        receipt = json.loads(self.receipt.read_text())
        receipt['overlays'] = [stage for stage in receipt['overlays'] if stage['name'] != 'iop-heap']
        receipt['overlay_scripts'] = [stage for stage in receipt['overlay_scripts']
                                      if stage['name'] != 'iop-heap']
        self.receipt.write_text(json.dumps(receipt))
        self.assertEqual(self.verify(), 1)

    def test_create_rejects_omitted_iop_stage(self):
        self.overlays = [stage for stage in self.overlays if not stage.startswith('iop-heap=')]
        self.scripts = [entry for entry in self.scripts if entry[0] != 'iop-heap']
        with self.assertRaises(AssertionError):
            self.create()
        self.assertFalse(self.receipt.exists())

    def test_iop_stage_requires_callback_and_matching_script(self):
        stages = [{'name': 'callback-stack-main-reservation', 'active': False},
                  {'name': 'iop-heap', 'active': True}]
        scripts = [{'name': stage['name']} for stage in stages]
        with self.assertRaises(RuntimeError):
            identity.check_overlay_pairing(stages, scripts)
        stages[0]['active'] = True
        with self.assertRaises(RuntimeError):
            identity.check_overlay_pairing(stages, scripts[:1])

    def test_x86_64_records_inactive_jit_without_git_resolution(self):
        self.create_x86()
        data = json.loads(self.receipt.read_text())
        self.assertEqual(data['jit'], {
            'active': False,
            'architecture': 'x86_64',
            'selection': 'inactive on non-ARM64 effective architecture',
        })
        self.assertEqual(self.verify(), 0)

    def test_arm64_records_no_jit_and_pins_sse2neon(self):
        self.create()
        data = json.loads(self.receipt.read_text())
        self.assertFalse(data['jit']['active'])
        self.assertIn('sse2neon', data['jit'])
        self.assertNotIn('vixl', data['jit'])
        self.assertEqual(self.verify(), 0)

    def test_arm64_requires_jit_inputs(self):
        argv = ['create', '--source-root', str(self.source), '--build-dir', str(self.build),
                '--binary', str(self.binary), '--generation-manifest', str(self.generation),
                '--generated-dir', str(self.generated), '--bridge-manifest', str(self.bridge),
                '--bridge-manifest-verifier', str(self.bridge_verifier), '--bridge-manifest-verifier-text', str(self.bridge_verifier),
                '--resource-package-spec', str(self.package_spec), '--resource-package-root', str(self.package_root),
                '--sdl-manifest', str(self.sdl_manifest), '--architecture', 'arm64',
                '--abi-header', str(self.abi), '--output', str(self.receipt)]
        for value in self.overlays:
            argv += ['--overlay-stage', value]
        for name, value in self.scripts:
            argv += ['--overlay-script', f'{name}={value}']
        with mock.patch.object(identity, 'metadata_from_ninja', return_value=self.metadata), \
             mock.patch.object(sys, 'argv', ['receipt'] + argv):
            self.assertEqual(identity.main(), 1)

    def test_changed_byte_and_dirty_state_fail(self):
        self.create()
        self.binary.write_bytes(b'changed')
        with mock.patch.object(sys, 'argv', ['receipt', 'verify', '--receipt', str(self.receipt)]):
            self.assertEqual(identity.main(), 1)
        self.binary.write_bytes(b'binary')
        write(self.source / 'tracked', b'dirty')
        self.assertEqual(self.verify(), 1)

    def test_published_package_wrong_node_kinds_fail(self):
        self.create()
        (self.package_root / 'unexpected').mkdir()
        self.assertEqual(self.verify(), 1)
        (self.package_root / 'unexpected').rmdir()
        bridge = self.package_root / 'bridge.dylib'
        bridge.unlink()
        os.symlink(self.binary, bridge)
        self.assertEqual(self.verify(), 1)

    def test_published_package_parent_symlink_is_rejected_without_mutation(self):
        self.create()
        parent = self.package_root.parent
        outside = self.build / 'outside-package-parent'
        parent.rename(outside)
        sentinel = outside / 'sentinel'
        sentinel.write_bytes(b'outside remains untouched')
        os.symlink(outside, parent)
        self.assertEqual(self.verify(), 1)
        self.assertEqual(sentinel.read_bytes(), b'outside remains untouched')

    def test_untracked_byte_state_fails(self):
        write(self.source / 'untracked-name-with-space', b'first')
        self.create()
        write(self.source / 'untracked-name-with-space', b'second')
        self.assertEqual(self.verify(), 1)

    def test_missing_field_overlay_order_and_build_metadata_fail(self):
        self.create()
        data = json.loads(self.receipt.read_text())
        del data['sdl']
        self.receipt.write_text(json.dumps(data))
        self.assertEqual(self.verify(), 1)
        self.create()
        data = json.loads(self.receipt.read_text())
        data['overlays'][0], data['overlays'][1] = data['overlays'][1], data['overlays'][0]
        self.receipt.write_text(json.dumps(data))
        self.assertEqual(self.verify(), 1)
        self.create()
        data = json.loads(self.receipt.read_text())
        data['overlays'].pop()
        self.receipt.write_text(json.dumps(data))
        self.assertEqual(self.verify(), 1)
        self.create()
        (self.build / 'build.ninja').write_bytes(b'changed build metadata')
        self.assertEqual(self.verify(), 1)

    @unittest.skipUnless(shutil.which('ninja') and shutil.which('c++'), 'requires Ninja and C++')
    def test_real_ninja_metadata_create_verify_and_command_tamper(self):
        source = self.build / 'real.cpp'; header = self.build / 'real.h'
        source.write_text('#include "real.h"\nint main() { return value; }\n')
        header.write_text('constexpr int value = 0;\n')
        (self.build / 'build.ninja').write_text(
            'rule cxx\n  command = c++ -std=c++17 -MMD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n'
            'rule link\n  command = c++ $in -o $out\n'
            'build real.o: cxx real.cpp\nbuild rrv-product: link real.o\n')
        subprocess.run(['ninja', '-C', str(self.build), 'rrv-product'], check=True)
        (self.build / 'CMakeCache.txt').write_text(
            'RRV_IOP_HEAP_ISOLATION:BOOL=OFF\nRRV_RPC_MEMORY_SAFETY:BOOL=OFF\n')
        (self.build / 'compile_commands.json').write_text(json.dumps([{'file': str(source)}]))
        (self.build / 'product-no-raylib.json').write_text('{}')
        self.binary = self.build / 'rrv-product'
        argv = ['create', '--source-root', str(self.source), '--build-dir', str(self.build),
                '--binary', str(self.binary), '--generation-manifest', str(self.generation),
                '--generated-dir', str(self.generated), '--bridge-manifest', str(self.bridge),
                '--bridge-manifest-verifier', str(self.bridge_verifier), '--bridge-manifest-verifier-text', str(self.bridge_verifier),
                '--resource-package-spec', str(self.package_spec), '--resource-package-root', str(self.package_root),
                '--sdl-manifest', str(self.sdl_manifest), '--architecture', 'x86_64',
                '--abi-header', str(self.abi), '--output', str(self.receipt)]
        for value in self.overlays: argv += ['--overlay-stage', value]
        for name, value in self.scripts: argv += ['--overlay-script', f'{name}={value}']
        with mock.patch.object(sys, 'argv', ['receipt'] + argv):
            self.assertEqual(identity.main(), 0)
        with mock.patch.object(sys, 'argv', ['receipt', 'verify', '--receipt', str(self.receipt)]):
            self.assertEqual(identity.main(), 0)
        (self.build / 'build.ninja').write_text((self.build / 'build.ninja').read_text() + '# tamper\n')
        with mock.patch.object(sys, 'argv', ['receipt', 'verify', '--receipt', str(self.receipt)]):
            self.assertEqual(identity.main(), 1)


if __name__ == '__main__':
    unittest.main()
