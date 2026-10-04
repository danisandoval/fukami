#!/usr/bin/env python3
"""Golden test: the launcher's environment equals what the old bash translation produced.

tests/fixtures/launch_env/golden.json was recorded (scripts/record_launch_env_golden.py) from
scripts/run_gate4_product.sh while it still parsed rrv.ini and the flags itself. This test runs the
launcher as it is now (RRV_LAUNCH_DRY_RUN=1, the translation done by the `--print-launch-env` entry point,
here the standalone rrv-launch-env build of it, RRV_LAUNCH_ENV_TOOL) over the same matrix and requires the same
ordered environment, and the same exit codes and first error line for the recorded error cases.
It also covers the [env] policy: a developer-only variable needs `[developer] enabled = true`.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GOLDEN = json.loads((ROOT / "tests/fixtures/launch_env/golden.json").read_text())
SCRIPT = ROOT / "scripts/run_gate4_product.sh"
TOOL = os.environ.get("RRV_LAUNCH_ENV_TOOL", "")


def normalise(lines, workload: Path, ini: Path):
    out = []
    for line in lines:
        line = line.replace(str(ini), "<INI>").replace(str(workload), "<WORKLOAD>").replace(str(ROOT), "<ROOT>")
        line = re.sub(r"<ROOT>/local/product/sessions/[^/]+", "<SESSION>", line)
        line = re.sub(r"<ROOT>/runtime/[^/]+/lib", "<LIBS>", line)
        out.append(line)
    return out


@unittest.skipUnless(TOOL and Path(TOOL).is_file(), "RRV_LAUNCH_ENV_TOOL (rrv-launch-env) is not built")
class LaunchEnvGolden(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dir = Path(self.temp.name).resolve()
        self.workload = self.dir / "workload"
        self.workload.mkdir()
        (self.workload / "bound-workload.txt").write_text("storage persistent_user_card\n")
        self.replay = self.dir / "replay"
        (self.replay / "mc-initial").mkdir(parents=True)
        (self.replay / "bound-workload.txt").write_text("storage persistent_user_card\n")
        self.env = {"HOME": GOLDEN["home"], "PATH": GOLDEN["path"], "TMPDIR": GOLDEN["tmpdir"],
                    "RRV_LAUNCH_DRY_RUN": "1", "RRV_LAUNCH_ENV_TOOL": TOOL}

    def tearDown(self):
        self.temp.cleanup()

    def launch(self, name: str, text: str, args, replay=False, extra_env=None):
        ini = self.dir / (re.sub(r"\W+", "_", name) + ".ini")
        ini.write_text(text)
        command = [str(SCRIPT), "--config", str(ini), *args]
        command += ["--replay", str(self.replay)] if replay else ["--workload", str(self.workload)]
        done = subprocess.run(command, env={**self.env, **(extra_env or {})}, capture_output=True, text=True)
        return done, ini

    def test_every_recorded_case_matches_the_old_launcher(self):
        self.assertGreaterEqual(len(GOLDEN["cases"]), 250)
        for case in GOLDEN["cases"]:
            with self.subTest(case["name"]):
                text = case["ini"]
                if case["needs_developer"]:
                    text += "[developer]\nenabled = true\n"
                done, ini = self.launch(case["name"], text, case["args"], case["replay"])
                self.assertEqual(done.returncode, 0, done.stderr)
                lines = normalise(done.stdout.splitlines(), self.replay if case["replay"] else self.workload, ini)
                self.assertEqual(lines, case["env"])

    def test_recorded_errors_still_fail_the_same_way(self):
        for case in GOLDEN["errors"]:
            with self.subTest(case["name"]):
                done, ini = self.launch(case["name"], case["ini"], case["args"])
                self.assertEqual(done.returncode, case["returncode"], done.stderr)
                if case["first_stderr_line"].startswith(("<INI>", "--from")):
                    first = done.stderr.splitlines()[0].replace(str(ini), "<INI>")
                    self.assertEqual(first, case["first_stderr_line"])

    def test_env_policy_needs_developer_mode(self):
        done, _ = self.launch("policy", "[env]\nRRV_VU_AOT_RECORD = /tmp/x\n", [])
        self.assertEqual(done.returncode, 2)
        self.assertIn("developer setting", done.stderr)
        self.assertIn("[developer]", done.stderr)
        done, _ = self.launch("policy ok", "[env]\nRRV_PCSX2_GS_HUD_SCALE = 0.5\n", [])
        self.assertEqual(done.returncode, 0, done.stderr)
        done, _ = self.launch("policy ini", "[developer]\nenabled = true\n[env]\nRRV_VU_AOT_RECORD = /tmp/x\n", [])
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("RRV_VU_AOT_RECORD=/tmp/x", done.stdout.splitlines())
        done, _ = self.launch("policy env", "[env]\nRRV_VU_AOT_RECORD = /tmp/x\n", [], extra_env={"RRV_DEVELOPER": "1"})
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("RRV_VU_AOT_RECORD=/tmp/x", done.stdout.splitlines())

    def test_paths_from_the_ini_and_the_flags(self):
        # --runtime is a package NAME (the launcher looks in runtime/NAME); --workload/--replay/ELF are paths.
        done, ini = self.launch("runtime flag", "", ["--runtime", "game001-test-v1"])
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn(f"DYLD_LIBRARY_PATH={ROOT}/runtime/game001-test-v1/lib", done.stdout.splitlines())
        done, _ = self.launch("runtime ini", "[paths]\nruntime = game001-ini-v2\n", [])
        self.assertIn(f"DYLD_LIBRARY_PATH={ROOT}/runtime/game001-ini-v2/lib", done.stdout.splitlines())
        done, _ = self.launch("runtime flag wins", "[paths]\nruntime = game001-ini-v2\n", ["--runtime", "game001-flag-v3"])
        self.assertIn(f"DYLD_LIBRARY_PATH={ROOT}/runtime/game001-flag-v3/lib", done.stdout.splitlines())
        # The config the launcher reads back.
        ini = self.dir / "cfg.ini"
        ini.write_text("[paths]\nworkload = local/x\nruntime = r1\nelf = /abs/elf\n")
        done = subprocess.run([TOOL, "--print-launch-config", str(ini), "--root", str(ROOT), "--flags", "--from", "7"],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        config = dict(line.split("=", 1) for line in done.stdout.splitlines())
        self.assertEqual((config["runtime"], config["workload"], config["elf"], config["from"]),
                         ("r1", f"{ROOT}/local/x", "/abs/elf", "7"))
        elf = self.dir / "game.elf"
        elf.write_text("x")
        done = subprocess.run([TOOL, "--print-launch-config", str(ini), "--root", str(ROOT), "--flags", str(elf)],
                              capture_output=True, text=True)
        self.assertEqual(dict(line.split("=", 1) for line in done.stdout.splitlines())["elf"], str(elf))
        done = subprocess.run([TOOL, "--print-launch-config", str(ini), "--root", str(ROOT), "--flags", str(elf), "/b"],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 2)

    def test_a_setting_in_the_wrong_section_is_an_error(self):
        done, _ = self.launch("wrong section", "[game]\nratio = 4:3\n", [])
        self.assertEqual(done.returncode, 2)
        self.assertIn("'ratio' belongs under [display]", done.stderr)

    def test_runtime_without_the_entry_point_is_refused_clearly(self):
        # An old runtime answers --print-launch-* like a missing ELF path; no fallback tool is available.
        old = self.dir / "old-runtime"
        old.write_text("#!/bin/sh\necho 'usage: old <user-owned-boot-elf>' >&2\nexit 2\n")
        old.chmod(0o755)
        env = {**self.env, "RRV_LAUNCH_ENV_TOOL": str(old), "RRV_LAUNCH_ENV_FALLBACK": str(self.dir / "absent")}
        done = subprocess.run([str(SCRIPT), "--workload", str(self.workload)], env=env, capture_output=True, text=True)
        self.assertEqual(done.returncode, 3)
        self.assertIn("no launcher entry point", done.stderr)
        # With a fallback tool (the developer checkout's build) the same old runtime is launched through it.
        env["RRV_LAUNCH_ENV_FALLBACK"] = TOOL
        done = subprocess.run([str(SCRIPT), "--workload", str(self.workload)], env=env, capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("RRV_GATE3_REALTIME=1", done.stdout.splitlines())


if __name__ == "__main__":
    unittest.main()
