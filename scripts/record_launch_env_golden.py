#!/usr/bin/env python3
"""Record the launcher's environment for a matrix of rrv.ini files and flags.

This was run ONCE against the bash translation in scripts/run_gate4_product.sh (RRV_LAUNCH_DRY_RUN=1)
at commit bb8c7a3's successor, before that translation was replaced by the product binary's
`--print-launch-env` (Task 6 of RRV_NEXT_STEPS_PLAN.md). The result is tests/fixtures/launch_env/golden.json,
which tests/test_launch_env_golden.py compares with the new path. It cannot run against the new launcher (it
only exists to document how the fixture was made) and refuses to overwrite an existing fixture.

Cases: every setting with its default and every legal value (several spellings of booleans), the ratio x hud
and ratio x aspect grids, the timing combinations, every command-line flag, [env] lines, a replay, whitespace,
CRLF, upper-case names, duplicate keys and the repository's rrv.ini.
"""
from __future__ import annotations

import itertools
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests/fixtures/launch_env/golden.json"
SCRIPT = ROOT / "scripts/run_gate4_product.sh"

BOOLS = ["true", "false", "yes", "no", "on", "off", "1", "0"]
RATIOS = ["4:3", "16:9", "16:10", "21:9"]
HUDS = ["stretch", "fixed", "race", "no47"]
ASPECTS = ["", "auto", "4:3", "16:9", "16:10", "21:9", "stretch"]


def ini(**sections: dict[str, str]) -> str:
    text = ""
    for name, keys in sections.items():
        text += f"[{name}]\n" + "".join(f"{k} = {v}\n" for k, v in keys.items())
    return text


def cases() -> list[dict]:
    out: list[dict] = []

    def add(name, text="", args=(), developer=False, replay=False):
        out.append({"name": name, "ini": text, "args": list(args), "needs_developer": developer, "replay": replay})

    add("empty ini")
    add("repo rrv.ini", (ROOT / "rrv.ini").read_text())
    for key, section in (("fullscreen", "display"), ("integer_scaling", "display"), ("present_pacing", "display"),
                         ("aa1", "rendering"), ("fxaa", "rendering"), ("mipmap", "rendering"),
                         ("analog", "input"), ("rumble", "input"), ("fast_unpack", "game"), ("native_code", "game"),
                         ("unpaced", "timing"), ("inline", "timing"), ("headless", "timing")):
        for value in BOOLS:
            add(f"{section}.{key}={value}", ini(**{section: {key: value}}))
    for ratio in RATIOS:
        add(f"ratio={ratio}", ini(display={"ratio": ratio}))
        for hud in HUDS:
            add(f"ratio={ratio} hud={hud}", ini(display={"ratio": ratio, "hud": hud}))
        for aspect in ASPECTS:
            add(f"ratio={ratio} aspect={aspect or 'empty'}", ini(display={"ratio": ratio, "aspect": aspect}))
    for aspect in ASPECTS:
        add(f"aspect={aspect or 'empty'}", ini(display={"aspect": aspect}))
    for window in ("", "1280x720", "1920x1080", "800x600"):
        add(f"window={window or 'empty'}", ini(display={"window": window}))
    for mode in ("full", "field"):
        add(f"render_mode={mode}", ini(rendering={"render_mode": mode}))
    for scale in range(1, 9):
        add(f"scale={scale}", ini(rendering={"scale": str(scale)}))
    for cas in (0, 1, 35, 100):
        add(f"cas={cas}", ini(rendering={"cas": str(cas)}))
    for aniso in (0, 2, 4, 8, 16):
        add(f"aniso={aniso}", ini(rendering={"aniso": str(aniso)}))
    for lod in ("1", "1.0", "0.5", "2.5", "4", "7.25", "16"):
        add(f"car_lod={lod}", ini(game={"car_lod": lod}))
    for distance in (0, 1, 2, 4, 16):
        add(f"draw_distance={distance}", ini(game={"draw_distance": str(distance)}))
    for start in ("", "0", "5000"):
        add(f"from={start or 'empty'}", ini(timing={"from": start}))
    for headless, unpaced, inline in itertools.product(("true", "false"), repeat=3):
        add(f"timing h={headless} u={unpaced} i={inline}",
            ini(timing={"headless": headless, "unpaced": unpaced, "inline": inline}))
    add("everything non-default",
        ini(display={"ratio": "21:9", "hud": "race", "aspect": "stretch", "window": "1280x720", "fullscreen": "true",
                     "integer_scaling": "true", "present_pacing": "false"},
            rendering={"render_mode": "field", "scale": "2", "aa1": "false", "fxaa": "true", "cas": "35", "aniso": "16",
                       "mipmap": "false"},
            input={"analog": "false", "rumble": "false"},
            game={"car_lod": "1", "draw_distance": "4", "fast_unpack": "false", "native_code": "false"},
            timing={"unpaced": "true", "inline": "true", "headless": "true"}))
    # [env]
    add("env allowed", ini(env={"RRV_PCSX2_GS_HUD_SCALE": "0.75", "RRV_PCSX2_GS_VSYNC": "0"}))
    add("env override of a setting", ini(display={"ratio": "21:9"}, env={"RRV_RR5_WIDESCREEN": "16:9"}))
    add("env developer", ini(env={"RRV_FOO": "1", "RRV_FRONTEND_DIAG": "1"}), developer=True)
    # reader quirks
    add("whitespace and comments", "# c\n; c\n\n  [ Display ]  \n  RATIO   =   21:9  \n[env]\n  RRV_PCSX2_GS_HUD_SCALE=0.5\n")
    add("crlf", "[display]\r\nratio = 16:10\r\n[rendering]\r\nscale = 3\r\n")
    add("duplicate key later wins", ini(display={"ratio": "4:3"}) + "[display]\nratio = 21:9\n")
    add("no trailing newline", "[game]\ncar_lod = 2")
    add("hud no47 alias", ini(display={"ratio": "21:9", "hud": "no47"}))
    # command-line flags (override the ini)
    flags = [
        ["--headless"], ["--unpaced"], ["--inline"], ["--threaded"], ["--no-aa1"], ["--fxaa"], ["--no-mipmap"],
        ["--no-analog"], ["--no-rumble"], ["--no-fast-unpack"], ["--no-native-code"], ["--fullscreen"],
        ["--integer-scaling"], ["--no-present-pacing"], ["--widescreen"], ["--stretched-hud"],
        ["--cas", "50"], ["--aniso", "8"], ["--car-lod", "2.5"], ["--draw-distance", "3"],
        ["--ratio", "4:3"], ["--ratio", "21:9"], ["--hud", "fixed"], ["--hud", "race"], ["--hud", "no47"],
        ["--aspect", "auto"], ["--aspect", "stretch"], ["--window", "1600x900"], ["--scale", "2"],
        ["--render-mode", "field"], ["--ratio", "16:10", "--hud", "fixed", "--aspect", "16:9"],
        ["--headless", "--unpaced", "--no-aa1", "--scale", "1", "--car-lod", "1"],
    ]
    for args in flags:
        add("flags " + " ".join(args), args=args)
    add("flag overrides ini", ini(display={"ratio": "21:9"}, rendering={"scale": "8", "aa1": "true"}),
        ["--ratio", "4:3", "--scale", "2", "--no-aa1"])
    add("fast-forward flag", args=["--from", "3000"])
    add("replay", ini(timing={"headless": "true"}), replay=True)
    add("replay from", args=["--from", "1200"], replay=True)
    return out


ERRORS = [
    ("bad ratio", "[display]\nratio = 5:4\n", []),
    ("bad hud", "[display]\nhud = wide\n", []),
    ("bad aspect", "[display]\naspect = 2:1\n", []),
    ("bad window", "[display]\nwindow = big\n", []),
    ("bad bool", "[display]\nfullscreen = maybe\n", []),
    ("bad render_mode", "[rendering]\nrender_mode = half\n", []),
    ("scale 9", "[rendering]\nscale = 9\n", []),
    ("cas 101", "[rendering]\ncas = 101\n", []),
    ("aniso 3", "[rendering]\naniso = 3\n", []),
    ("bad car_lod", "[game]\ncar_lod = abc\n", []),
    ("draw_distance 17", "[game]\ndraw_distance = 17\n", []),
    ("bad from", "[timing]\nfrom = soon\n", []),
    ("unknown key", "[game]\nspeed = 1\n", []),
    ("no equals", "[game]\njunk\n", []),
    ("lower-case env name", "[env]\nrrv_foo = 1\n", []),
    ("from with unpaced", "[timing]\nfrom = 100\nunpaced = true\n", []),
    ("flag bad ratio", "", ["--ratio", "5:4"]),
    ("flag bad scale", "", ["--scale", "0"]),
    ("flag bad aniso", "", ["--aniso", "3"]),
    ("flag missing value", "", ["--cas"]),
    ("flag unknown", "", ["--turbo"]),
]


def normalise(lines: list[str], root: Path, workload: Path, ini_path: Path) -> list[str]:
    result = []
    for line in lines:
        line = line.replace(str(ini_path), "<INI>").replace(str(workload), "<WORKLOAD>").replace(str(root), "<ROOT>")
        line = re.sub(r"<ROOT>/local/product/sessions/[^/]+", "<SESSION>", line)
        line = re.sub(r"<ROOT>/runtime/[^/]+/lib", "<LIBS>", line)
        result.append(line)
    return result


def main() -> int:
    if OUT.exists():
        print(f"{OUT} exists; refusing to overwrite (the fixture is the record of the old launcher)", file=sys.stderr)
        return 1
    recorded = []
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory).resolve()
        workload = temp / "workload"
        workload.mkdir()
        (workload / "bound-workload.txt").write_text("storage persistent_user_card\n")
        replay = temp / "replay"
        (replay / "mc-initial").mkdir(parents=True)
        (replay / "bound-workload.txt").write_text("storage persistent_user_card\n")
        home, path, tmp = "/home/test", "/usr/bin:/bin", "/tmp/rrv-test"
        for index, case in enumerate(cases()):
            config = temp / f"c{index}.ini"
            config.write_text(case["ini"])
            command = [str(SCRIPT), "--config", str(config), *case["args"]]
            command += ["--replay", str(replay)] if case["replay"] else ["--workload", str(workload)]
            env = {"HOME": home, "PATH": path, "TMPDIR": tmp, "RRV_LAUNCH_DRY_RUN": "1"}
            done = subprocess.run(command, env=env, capture_output=True, text=True)
            if done.returncode != 0:
                print(f"{case['name']}: old launcher failed ({done.returncode}): {done.stderr}", file=sys.stderr)
                return 1
            lines = normalise(done.stdout.splitlines(), ROOT, workload if not case["replay"] else replay, config)
            recorded.append({**case, "env": lines})
    errors = []
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory).resolve()
        workload = temp / "workload"
        workload.mkdir()
        (workload / "bound-workload.txt").write_text("storage persistent_user_card\n")
        for index, (name, text, args) in enumerate(ERRORS):
            config = temp / f"e{index}.ini"
            config.write_text(text)
            env = {"HOME": home, "PATH": path, "TMPDIR": tmp, "RRV_LAUNCH_DRY_RUN": "1"}
            done = subprocess.run([str(SCRIPT), "--config", str(config), *args, "--workload", str(workload)],
                                  env=env, capture_output=True, text=True)
            first = done.stderr.splitlines()[0].replace(str(config), "<INI>") if done.stderr else ""
            errors.append({"name": name, "ini": text, "args": args, "returncode": done.returncode,
                           "first_stderr_line": first})
    OUT.write_text(json.dumps({"kind": "rrv-launch-env-golden", "schema_version": 1,
                               "recorded_from": "scripts/run_gate4_product.sh (bash translation), RRV_LAUNCH_DRY_RUN=1",
                               "placeholders": ["<ROOT>", "<WORKLOAD>", "<SESSION>", "<LIBS>", "<INI>"],
                               "home": home, "path": path, "tmpdir": tmp, "cases": recorded, "errors": errors}, indent=1) + "\n")
    print(f"recorded {len(recorded)} cases -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
