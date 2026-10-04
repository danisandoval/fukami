#!/usr/bin/env python3
"""Pinned-producer product-host specialization and negative composition checks."""
from pathlib import Path
import sys
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import product_host_overlay as overlay

SOURCE = ROOT / 'build-deps/ps2recomp-d52-compatible-v2/ps2xRuntime'


@unittest.skipUnless(SOURCE.is_dir(), 'requires separately prepared pinned producer')
class ProductHostOverlayTests(unittest.TestCase):
    def test_pins_and_no_legacy_dependency(self):
        overlay.verify(SOURCE)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'host'
            overlay.generate(SOURCE, out)
            cmake = (out / 'CMakeLists.txt').read_text()
            self.assertNotIn('raylib', cmake.lower())
            self.assertNotIn('ps2EntryRunner', cmake)
            self.assertNotIn('host_pad_gamecontroller', cmake)
            self.assertIn('-ffp-contract=off', cmake)
            self.assertNotIn('raylib', (out / 'include/ps2_host_backend.h').read_text())
            runtime_header = (out / 'include/ps2_runtime.h').read_text()
            self.assertIn('rrv_guest_terminal_outcome.h', runtime_header)
            self.assertIn('RuntimeTerminalOutcomeHandle m_rrvTerminalOutcome', runtime_header)
            self.assertIn('terminalOutcomeHandle()', runtime_header)
            worker = (out / 'src/lib/Kernel/Syscalls/Thread.cpp').read_text()
            self.assertIn('RejectedWorkerDispatch', worker)
            self.assertIn('unregistered StartThread initial entry', worker)
            self.assertIn('unregistered StartThread later dispatch target', worker)
            self.assertIn('WorkerGuestException', worker)
            initial_guard = worker[worker.index('if (!runtime || !runtime->hasFunction(info->entry))'):]
            initial_guard = initial_guard[:initial_guard.index('if (runtime->isStopRequested())')]
            self.assertEqual(worker.count('unregistered StartThread initial entry'), 1)
            self.assertEqual(worker.count('runtime->hasFunction(info->entry)'), 1)
            self.assertIn('if (runtime)', initial_guard)
            self.assertIn('terminalOutcomeHandle()', initial_guard)
            self.assertLess(initial_guard.index('if (runtime)'),
                            initial_guard.index('runtime->terminalOutcomeHandle()'))
            self.assertEqual(initial_guard.count('recordFailure('), 1)
            self.assertNotIn('info->started', initial_guard)
            self.assertNotIn('info->status', initial_guard)
            self.assertLess(initial_guard.index('recordFailure('),
                            initial_guard.index('runtime->requestStop()'))
            self.assertLess(initial_guard.index('runtime->requestStop()'),
                            initial_guard.index('setReturnS32(ctx, KE_ERROR)'))
            self.assertLess(initial_guard.index('setReturnS32(ctx, KE_ERROR)'),
                            initial_guard.index('return;'))
            self.assertLess(worker.index('runtime->hasFunction(info->entry)'),
                            worker.index('g_activeThreads.fetch_add'))
            worker_loop = worker[worker.index('while (runtime && !runtime->isStopRequested())'):]
            self.assertLess(worker_loop.index('if (!runtime->hasFunction(pc))'),
                            worker_loop.index('runtime->lookupFunction(pc)'))
            cmake_contract = (ROOT / 'cmake' / 'RrvProductHost.cmake').read_text()
            for configured_input in (
                    'include/ps2_runtime.h',
                    'src/lib/Kernel/Syscalls/Thread.cpp',
                    'src/rrv_guest_terminal_outcome.h'):
                self.assertIn(configured_input, cmake_contract)
            self.assertIn('src/lib/Kernel/Syscalls/Common.h', cmake_contract)
            self.assertIn('src/lib/Kernel/Syscalls/Thread.h', cmake_contract)
            self.assertIn('src/lib/Kernel/Syscalls")', cmake)
            audio = (out / 'src/lib/ps2_audio.cpp').read_text()
            for forbidden in ('LoadWaveFromMemory', 'LoadSoundFromWave', 'IsSoundPlaying', 'UnloadSound', 'ps2_host_backend.h'):
                self.assertNotIn(forbidden, audio)
            # Guest-side voice decisions remain byte-identical.
            original = (SOURCE / 'src/lib/ps2_audio.cpp').read_text()
            begin, end = 'void PS2AudioBackend::onVagTransfer(', 'void PS2AudioBackend::pruneFinishedSounds()'
            self.assertEqual(original[original.index(begin):original.index(end)], audio[audio.index(begin):audio.index(end)])
            first = (out / 'product-host-overlay-manifest.json').read_bytes()
            overlay.generate(SOURCE, out)
            self.assertEqual(first, (out / 'product-host-overlay-manifest.json').read_bytes())

    def test_all_known_runtime_compositions(self):
        original = (SOURCE / 'src/lib/ps2_runtime.cpp').read_text()
        variants = overlay.known_runtime_inputs(original)
        self.assertEqual(len(variants), 8)
        for name, value in variants.items():
            with self.subTest(name=name):
                result = overlay.runtime(value)
                for forbidden in ('InitAudioDevice', 'CloseAudioDevice', 'Texture2D', 'BeginDrawing', 'WindowShouldClose', '#include "ps2_host_backend.h"'):
                    self.assertNotIn(forbidden, result)
                self.assertIn('serviceDirectPresentation();', result)
                self.assertIn('RejectedMainDispatch', result)
                self.assertIn('unregistered main dispatch target', result)
                self.assertIn('recordMainCompletion', result)
                self.assertIn('MainGuestException', result)
                self.assertIn('HostRuntimeException', result)
                self.assertLess(result.index('RejectedMainDispatch'), result.index('RecompiledFunction fn = lookupFunction(pc);'))
                # A registered indirect/re-entry target remains untouched: only
                # the explicit !hasFunction branch records a rejected dispatch.
                dispatch = result.split('void PS2Runtime::dispatchLoop', 1)[1].split(
                    'void PS2Runtime::enterGuestExecution', 1)[0]
                self.assertIn('if (!hasFunction(pc))', dispatch)
                self.assertNotIn('recordFailure(\n                terminalOutcome()',
                                 dispatch[dispatch.index('RecompiledFunction fn = lookupFunction(pc);') + 1:])
                # Both direct presentation functions are outside the removed
                # legacy host code and must survive without a changed byte.
                start = value.index('void PS2Runtime::serviceDirectPresentation()')
                end = value.index('\nvoid ', start + 1)
                self.assertIn(value[start:end], result)

    @unittest.skipUnless(shutil.which('c++'), 'requires a C++ compiler')
    def test_disconnected_pad_allows_late_injection(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'pad.cpp').write_text(overlay.pad((SOURCE / 'src/lib/ps2_pad.cpp').read_text()))
            (tmp / 'main.cpp').write_text(r'''#include "runtime/ps2_pad.h"
#include <memory>
struct FakePad : HostPadBackend {
    HostPadState snapshot(unsigned port) override {
        return port == 0 ? HostPadState{true, 123, HostPadButtonCross} : HostPadState{};
    }
    HostPadCapabilities capabilities(unsigned port) const override {
        return port == 0 ? HostPadCapabilities{true, true, false} : HostPadCapabilities{};
    }
};
int main() {
    PSPadBackend pad;
    if (pad.snapshot().connected || pad.snapshot().connected || pad.capabilities().analogSticks) return 1;
    auto fake = std::make_shared<FakePad>();
    pad.setBackendForTesting(fake);
    if (!pad.snapshot().connected || pad.snapshot().deviceId != 123 ||
        pad.snapshot().pressedButtons != HostPadButtonCross || !pad.capabilities().analogSticks) return 2;
    if (pad.snapshot(1).connected) return 3;
    pad.clearBackendForTesting();
    if (pad.snapshot().connected || pad.capabilities().analogSticks) return 4;
    PSPadBackend::setExternalBackendForProcess(fake);
    PSPadBackend product;
    if (!product.snapshot().connected || product.snapshot().deviceId != 123) return 5;
    PSPadBackend::setExternalBackendForProcess({});
    return 0;
}
''')
            subprocess.run([shutil.which('c++'), '-std=c++20', '-pthread', '-I' + str(SOURCE / 'include'),
                            str(tmp / 'pad.cpp'), str(tmp / 'main.cpp'), '-o', str(tmp / 'test')],
                           check=True, capture_output=True, text=True)
            for mode in ('auto', 'raylib', 'keyboard', 'gamecontroller'):
                env = dict(os.environ, RRV_TEST_NEUTRAL_PAD='0', RRV_PAD_BACKEND=mode, RRV_PAD_DIAG='0')
                subprocess.run([str(tmp / 'test')], env=env, check=True, capture_output=True, text=True)

    def test_final_input_and_anchor_drift_rejected(self):
        runtime = SOURCE / 'src/lib/ps2_runtime.cpp'
        pad = SOURCE / 'src/lib/ps2_pad.cpp'
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            bad = tmp / 'runtime.cpp'
            bad.write_text(runtime.read_text().replace('serviceDirectPresentation();', 'serviceDirectPresentation(); // unknown edit'))
            with self.assertRaisesRegex(RuntimeError, 'unknown input'):
                overlay.finalize(SOURCE, bad, pad, tmp / 'out')
            with self.assertRaises(RuntimeError):
                overlay.runtime(runtime.read_text().replace('    const bool headlessDiagnostic = legacyHeadlessDiagnosticEnabled();\n', ''))
            overlay.finalize(SOURCE, runtime, pad, tmp / 'good')
            specialized_pad = (tmp / 'good/src/lib/ps2_pad.cpp').read_text()
            self.assertNotIn('make_shared<RaylibHostPadBackend>', specialized_pad)
            self.assertNotIn('make_shared<KeyboardHostPadBackend>', specialized_pad)
            self.assertNotIn('createGameControllerHostPadBackend()', specialized_pad)
        with self.assertRaisesRegex(RuntimeError, 'outside the immutable producer'):
            overlay.generate(SOURCE, SOURCE)


if __name__ == '__main__':
    unittest.main()
