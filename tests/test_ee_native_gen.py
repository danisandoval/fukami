#!/usr/bin/env python3
"""Tests for the native hot-function generator (tools/ee-native/ee_native_gen.py).

Synthetic generated functions (no game data) for the translation and the hook / probe points,
plus one check on the committed files: generated/rr5/native is exactly what the generator
produces from generated/rr5 and its own manifest.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / 'tools/ee-native/ee_native_gen.py'


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load('ee_native_gen', GENERATOR)

SYNTHETIC = '''#include "ps2_runtime_macros.h"

// Function: sub_00100000
void sub_00100000_0x100000(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
    switch (ctx->pc) {
        case 0x100008u: goto label_100008;
        default: break;
    }
    // 0x100000: 0x24020001  addiu       $v0, $zero, 0x1
    ctx->pc = 0x100000u;
    runtime->gate3CheckpointV1(ctx);
    runtime->gate3BeginInstructionV1(ctx);
    SET_GPR_S32(ctx, 2, 1);
label_100008:
    // 0x100008: 0x24420001  addiu       $v0, $v0, 0x1
    ctx->pc = 0x100008u;
    runtime->gate3CheckpointV1(ctx);
    runtime->gate3BeginInstructionV1(ctx);
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 2), 1));
label_100010:
    // 0x100010: 0x3e00008  jr          $ra
    ctx->pc = 0x100010u;
    runtime->gate3CheckpointV1(ctx);
    runtime->gate3BeginInstructionV1(ctx);
    ctx->pc = GPR_U32(ctx, 31);
    return;
}
'''


def synthetic(directory: Path, text: str = SYNTHETIC) -> Path:
    path = directory / 'sub_00100000_0x100000.cpp'
    path.write_text(text)
    return path


class TranslationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.src = synthetic(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def test_clock_calls_become_the_local_counter(self):
        name, code, words = gen.translate(self.src)
        self.assertEqual(name, 'sub_00100000_0x100000')
        self.assertEqual(words, [(0x100000, 0x24020001), (0x100008, 0x24420001), (0x100010, 0x03e00008)])
        self.assertNotIn('gate3CheckpointV1', code)
        self.assertNotIn('gate3BeginInstructionV1', code)
        self.assertEqual(code.count('rrvNative.ck();'), 3)
        self.assertEqual(code.count('rrvNative.begin();'), 3)
        self.assertIn('rrv_native::Clock rrvNative(runtime, ctx);', code)

    def test_no_hook_changes_nothing_else(self):
        plain = gen.translate(self.src)[1]
        self.assertNotIn('native hook', plain)
        self.assertNotIn('native probe', plain)

    def test_hook_follows_its_label_and_names_the_exit(self):
        hook = gen.parse_hook('0x100000:0x100008:0x100010=ns::take', False)
        self.assertEqual(hook, {'function': '0x100000', 'at': '0x100008', 'to': '0x100010', 'callee': 'ns::take'})
        code = gen.translate(self.src, [hook])[1].split('\n')
        at = code.index('label_100008:')
        self.assertEqual(code[at + 1],
                         '    if (ns::take(rdram, ctx, runtime, rrvNative)) goto label_100010; // native hook')
        # The label's own instruction still follows: a false return runs the generated statements.
        self.assertIn('0x100008', code[at + 2])
        plain = gen.translate(self.src)[1].split('\n')
        self.assertEqual([l for l in code if 'native hook' not in l], plain)

    def test_probe_follows_its_label(self):
        probe = gen.parse_hook('0x100000:0x100010=ns::look', True)
        code = gen.translate(self.src, [], [probe])[1].split('\n')
        at = code.index('label_100010:')
        self.assertEqual(code[at + 1], '    ns::look(rdram, ctx, runtime, rrvNative); // native probe')

    def test_hook_needs_labels_at_both_ends(self):
        for text in ('0x100000:0x100004:0x100010=f', '0x100000:0x100008:0x100014=f'):
            with self.assertRaises(ValueError):
                gen.translate(self.src, [gen.parse_hook(text, False)])
        with self.assertRaises(ValueError):
            gen.translate(self.src, [], [gen.parse_hook('0x100000:0x100004=f', True)])

    def test_bad_hook_text_is_refused(self):
        for text, probe in (('0x100000:0x100008=f', False), ('0x100000:0x100008:0x100010', False),
                            ('0x100000:0x100008:0x100010=f(x)', False), ('0x100000:0x100008:0x100010=f', True)):
            with self.assertRaises(SystemExit):
                gen.parse_hook(text, probe)

    def test_unknown_runtime_call_is_refused(self):
        bad = synthetic(Path(self.tmp.name), SYNTHETIC.replace('SET_GPR_S32(ctx, 2, 1);', 'runtime->handleSyscall(rdram, ctx);'))
        with self.assertRaises(ValueError):
            gen.translate(bad)


class CommandLineTests(unittest.TestCase):
    def run_gen(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(GENERATOR), *args], capture_output=True, text=True)

    def test_manifest_records_hooks_and_regenerates_from_itself(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'gen/output').mkdir(parents=True)
            synthetic(root / 'gen/output')
            (root / 'gen/gate3-accounted-generation-manifest.json').write_text('{}\n')
            done = self.run_gen('--generation', str(root / 'gen'), '--output', str(root / 'a'),
                                '--hook', '0x100000:0x100008:0x100010=ns::take',
                                '--probe', '0x100000:0x100010=ns::look', '0x100000')
            self.assertEqual(done.returncode, 0, done.stderr)
            manifest = json.loads((root / 'a/manifest.json').read_text())
            self.assertEqual(manifest['generator_version'], gen.GENERATOR_VERSION)
            self.assertEqual(manifest['hooks'], [{'function': '0x100000', 'at': '0x100008', 'to': '0x100010',
                                                  'callee': 'ns::take'}])
            self.assertEqual(manifest['probes'], [{'function': '0x100000', 'at': '0x100010', 'callee': 'ns::look'}])
            again = self.run_gen('--generation', str(root / 'gen'), '--output', str(root / 'b'),
                                 '--from-manifest', str(root / 'a/manifest.json'))
            self.assertEqual(again.returncode, 0, again.stderr)
            for name in ('rrv_ee_native.inc', 'manifest.json'):
                self.assertEqual((root / 'a' / name).read_bytes(), (root / 'b' / name).read_bytes(), name)

    def test_hook_for_a_function_that_is_not_generated_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'gen/output').mkdir(parents=True)
            synthetic(root / 'gen/output')
            (root / 'gen/gate3-accounted-generation-manifest.json').write_text('{}\n')
            done = self.run_gen('--generation', str(root / 'gen'), '--output', str(root / 'a'),
                                '--hook', '0x200000:0x100008:0x100010=f', '0x100000')
            self.assertNotEqual(done.returncode, 0)


@unittest.skipUnless((ROOT / 'generated/rr5/native/manifest.json').is_file(),
                     'generated game code is absent (run scripts/fukami_generate.py)')
class CommittedNativeFiles(unittest.TestCase):
    """generated/rr5/native is the generator's output for generated/rr5 and its own manifest."""

    def test_committed_inc_is_reproducible(self):
        native = ROOT / 'generated/rr5/native'
        with tempfile.TemporaryDirectory() as tmp:
            done = subprocess.run([sys.executable, str(GENERATOR), '--generation', str(ROOT / 'generated/rr5'),
                                   '--output', tmp, '--from-manifest', str(native / 'manifest.json')],
                                  capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            for name in ('rrv_ee_native.inc', 'manifest.json'):
                self.assertEqual(hashlib.sha256((Path(tmp) / name).read_bytes()).hexdigest(),
                                 hashlib.sha256((native / name).read_bytes()).hexdigest(), name)

    def test_hook_callees_are_declared_before_the_include(self):
        manifest = json.loads((ROOT / 'generated/rr5/native/manifest.json').read_text())
        patches = (ROOT / 'src/product/patches.cpp').read_text()
        include = patches.index('#include "rrv_ee_native.inc"')
        headers = [h for h in (ROOT / 'src/product').glob('*.h')
                   if f'#include "{h.name}"' in patches and patches.index(f'#include "{h.name}"') < include]
        for entry in manifest.get('hooks', []) + manifest.get('probes', []):
            namespace, _, function = entry['callee'].rpartition('::')
            declared = any(f'namespace {namespace}' in h.read_text() and f' {function}(' in h.read_text()
                           for h in headers)
            self.assertTrue(declared, f'{entry["callee"]} is not declared in a src/product header included '
                                      f'before rrv_ee_native.inc')


if __name__ == '__main__':
    unittest.main()
