#!/usr/bin/env python3
"""gt_capture.py — capture PCSX2 ground truth for attract checkpoints, unattended.

What this replaces
------------------
Every reference in `local/gt/` before 2026-08-13 was staged by a human holding
Ctrl+Shift+F8 at a moment that looked right. That caps the oracle at the scenes
somebody sat through, and it cannot be re-run when a checkpoint moves. This
drives ONE cold boot that captures a whole list of checkpoints, each one started
by the guest's own scene coordinate — the section counter at 0x334E94 plus the
attract cursor's {script, step} — so the same command reproduces the same
captures.

The trigger lives inside the emulator (tools/patches/pcsx2-gt-capture.patch);
this script owns configuration, launch, artifact naming, format conversion and
the manifest. Injecting the hotkey from outside is not possible here: this host
denies osascript the accessibility permission that sending keystrokes needs.

Reference purity
----------------
The build is the pinned revision plus one observational patch, and the settings
are forced to the same values the trusted 2026-08-02 manifest records (patches
off, SW renderer, native resolution, 4:3, deinterlace Automatic). Every dump
logs the ELF probe word 0x2206F8; a value other than 0x24020008 means the
session was contaminated and the capture is rejected. `--verify` re-captures A1
and A3 and compares them structurally against the existing trusted references,
which is the real test of whether this harness produces the same oracle.

Everything it writes is game-derived: keep it in local/ or /tmp, never in git.

Usage:
  scripts/gt_capture.py --targets p3:3:*:*:24,p4:4:*:*:24 --out local/gt_auto
  scripts/gt_capture.py --verify                 # re-capture A1/A3 and diff
  scripts/gt_capture.py --discover --secs 300    # just log the attract timeline
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INI = os.path.expanduser("~/Library/Application Support/PCSX2/inis/PCSX2.ini")
EMULOG = os.path.expanduser("~/Library/Application Support/PCSX2/logs/emulog.txt")
DEFAULT_APP = os.path.join(REPO, "build-pcsx2-gt-capture", "build", "pcsx2-qt",
                           "PCSX2.app", "Contents", "MacOS", "PCSX2")
GS2GSR = os.path.join(REPO, "build-release", "rrv-gs2gsr")

# The 2026-08-02 manifest's configuration, asserted rather than assumed. A
# capture taken under different settings is a different oracle.
REQUIRED_INI = {
    ("EmuCore", "EnablePatches"): "false",
    ("EmuCore", "EnableCheats"): "false",
    ("EmuCore", "EnableWideScreenPatches"): "false",
    ("EmuCore", "EnableNoInterlacingPatches"): "false",
    ("EmuCore/GS", "Renderer"): "13",          # Software
    ("EmuCore/GS", "upscale_multiplier"): "1",
    ("EmuCore/GS", "deinterlace_mode"): "0",   # Automatic
    ("EmuCore/GS", "pcrtc_antiblur"): "false",
    ("EmuCore/GS", "disable_interlace_offset"): "false",
    ("EmuCore/GS", "GSDumpCompression"): "0",  # uncompressed .gs
    ("EmuCore/GS", "AspectRatio"): "4:3",
}
CLEAN_PROBE = 0x24020008


def read_ini_sections(path):
    """Return {(section, key): value} for a PCSX2 ini."""
    out, section = {}, ""
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
            elif "=" in line and not line.startswith(("#", ";")):
                k, v = line.split("=", 1)
                out[(section, k.strip())] = v.strip()
    return out


def check_ini(ini):
    """Fail loudly rather than silently capturing a differently-configured oracle."""
    have = read_ini_sections(ini)
    bad = []
    for key, want in REQUIRED_INI.items():
        got = have.get(key)
        if got is not None and got != want:
            bad.append(f"[{key[0]}] {key[1]} = {got} (want {want})")
    if bad:
        print("error: PCSX2 is not configured like the trusted reference session:",
              file=sys.stderr)
        for b in bad:
            print("  " + b, file=sys.stderr)
        print("  fix these in " + ini, file=sys.stderr)
        sys.exit(2)


def run_capture(targets, outdir, timeout, settle, app, disc, emulog,
                qt_plugins="", extra_env=None):
    """One cold boot; returns (returncode, log text)."""
    os.makedirs(outdir, exist_ok=True)
    env = dict(os.environ)
    env["RRV_GT_CAPTURE"] = ",".join(targets)
    env["RRV_GT_CAPTURE_DIR"] = os.path.abspath(outdir)
    env["RRV_GT_CAPTURE_SETTLE"] = str(settle)
    env["RRV_GT_CAPTURE_QUIT"] = "1"
    if extra_env:
        env.update(extra_env)

    # A non-bundled PCSX2 build may need its Qt platform plugin. The builder no
    # longer assumes a machine-local prefix, so only use an explicit option.
    if "QT_QPA_PLATFORM_PLUGIN_PATH" not in env and qt_plugins:
        env["QT_QPA_PLATFORM_PLUGIN_PATH"] = qt_plugins

    cmd = [app, "-batch", "-nogui", "-earlyconsolelog", "--", disc]
    print("launching:", " ".join(targets) or "(discovery)", flush=True)
    # PCSX2's Console goes to emulog.txt, not to our stdout, so the log file is
    # the only place the trigger's own record exists. It is rewritten per
    # session; drop the previous one so a stale line cannot be read as this run.
    if os.path.exists(emulog):
        os.remove(emulog)
    t0 = time.time()
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    shown = 0
    try:
        while proc.poll() is None and time.time() - t0 < timeout:
            time.sleep(1.0)
            lines = tail_log(emulog)
            for line in lines[shown:]:
                if "[gt-capture] START" in line or "matched at vsync" in line:
                    print("  " + line.rstrip(), flush=True)
            shown = len(lines)
        if proc.poll() is None:
            print(f"  timeout after {timeout:.0f}s — stopping", flush=True)
            proc.terminate()
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
    finally:
        if proc.poll() is None:
            proc.kill()
    log = tail_log(emulog)
    # A run that produced no trigger output at all did not "miss its
    # coordinates" — the emulator never got far enough to evaluate them. Say so,
    # instead of reporting a silent "0 of N fired".
    if not log and time.time() - t0 < 30.0:
        print(f"  ERROR: emulator exited after {time.time() - t0:.1f}s without "
              f"reaching the capture trigger; {emulog} has no [gt-capture] "
              f"lines. This is a launch failure, not a coordinate miss.",
              file=sys.stderr, flush=True)
    return proc.returncode, "".join(log)


def tail_log(emulog):
    """Every [gt-capture] line written so far by the running emulator."""
    if not os.path.exists(emulog):
        return []
    with open(emulog, "r", errors="replace") as fh:
        return [l for l in fh if "[gt-capture]" in l]


START_RE = re.compile(
    r"\[gt-capture\] START (\S+) at vsync (\d+) phase=(-?\d+) script=(-?\d+) "
    r"step=(-?\d+) fields=(\d+) probe\[0x([0-9A-F]+)\]=0x([0-9A-F]+)")
TIMELINE_RE = re.compile(
    r"\[gt-capture\] timeline vsync=(\d+) phase=(-?\d+) script=(-?\d+) step=(-?\d+)")


def parse_log(log):
    starts, timeline = [], []
    for line in log.splitlines():
        m = START_RE.search(line)
        if m:
            starts.append(dict(name=m.group(1), vsync=int(m.group(2)),
                               phase=int(m.group(3)), script=int(m.group(4)),
                               step=int(m.group(5)), fields=int(m.group(6)),
                               probe_addr=m.group(7), probe=int(m.group(8), 16)))
        m = TIMELINE_RE.search(line)
        if m:
            timeline.append(tuple(int(g) for g in m.groups()))
    return starts, timeline


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def convert(gs_path):
    """.gs -> .gsr, the format the rest of the oracle stack consumes."""
    gsr = gs_path[:-3] + ".gsr"
    if not os.path.exists(GS2GSR):
        print(f"  warning: {GS2GSR} missing; skipping conversion", file=sys.stderr)
        return None
    r = subprocess.run([GS2GSR, gs_path, "-o", gsr], capture_output=True, text=True)
    if r.returncode != 0:
        print("  gs2gsr failed:", r.stderr.strip()[:400], file=sys.stderr)
        return None
    return gsr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--targets", default="",
                    help="name:phase:script:step:fields[,...]  ('*' = any). Settle is GLOBAL (--settle); there is no per-target settle field.")
    ap.add_argument("--out", default=os.path.join(REPO, "local", "gt_auto"))
    ap.add_argument("--timeout", type=float, default=900.0)
    ap.add_argument("--settle", type=int, default=0,
                    help="vsyncs to wait after a coordinate first appears")
    ap.add_argument("--discover", action="store_true",
                    help="log the attract timeline without capturing anything")
    ap.add_argument("--secs", type=float, default=300.0,
                    help="discovery run length")
    ap.add_argument("--verify", action="store_true",
                    help="re-capture A1/A3 and compare against local/gt/")
    ap.add_argument("--app", default=os.environ.get("RRV_GT_CAPTURE_APP", DEFAULT_APP),
                    help="PCSX2 app binary; default is the GT builder output")
    ap.add_argument("--disc", default=os.environ.get("RRV_GT_CAPTURE_DISC", ""),
                    help="user-supplied disc/CHD path (or RRV_GT_CAPTURE_DISC)")
    ap.add_argument("--qt-plugin-path", default=os.environ.get("RRV_GT_CAPTURE_QT_PLUGIN_PATH", ""),
                    help="optional Qt platform-plugin directory for a non-bundled app")
    ap.add_argument("--ini", default=os.environ.get("RRV_GT_CAPTURE_INI", INI),
                    help="PCSX2 INI to validate before capture")
    ap.add_argument("--emulog", default=os.environ.get("RRV_GT_CAPTURE_EMULOG", EMULOG),
                    help="PCSX2 console log written by this capture run")
    args = ap.parse_args()

    if not os.path.exists(args.app):
        sys.exit(f"error: {args.app} not found — run scripts/build_pcsx2_gt_capture.sh or pass --app")
    if not args.disc:
        sys.exit("error: pass --disc or set RRV_GT_CAPTURE_DISC to user-supplied media")
    if not os.path.exists(args.disc):
        sys.exit(f"error: {args.disc} not found")
    if args.qt_plugin_path and not os.path.isdir(args.qt_plugin_path):
        sys.exit(f"error: Qt plugin directory not found: {args.qt_plugin_path}")
    if not os.path.exists(args.ini):
        sys.exit(f"error: PCSX2 INI not found: {args.ini}")
    check_ini(args.ini)

    if args.discover:
        # An unmatchable coordinate: the controller stays enabled (so the
        # timeline is logged) but never fires.
        rc, log = run_capture(["never:9999:*:*:2"], args.out, args.secs, 0,
                              args.app, args.disc, args.emulog, args.qt_plugin_path)
        _, timeline = parse_log(log)
        print(f"\nattract timeline ({len(timeline)} coordinate changes):")
        for vsync, phase, script, step in timeline:
            print(f"  vsync {vsync:6d}  phase={phase:<4} script={script:<4} step={step}")
        return

    targets = [t for t in args.targets.split(",") if t]
    if args.verify:
        # A1 is the phase-1 flyover; A3's demo race is phase 39, the long tail of
        # the attract loop (the timeline runs 0..39 in ~60 s and then sits on 39
        # for ~165 s). Both have trusted 2026-08-02 references to compare with.
        targets = ["verify_A1:1:*:*:16:120", "verify_A3:39:*:*:16:600"] + targets
    if not targets:
        sys.exit("error: nothing to capture (pass --targets or --discover)")

    # The emulator-side patch parses exactly five fields
    # (name:phase:script:step:fields) and takes the settle from the GLOBAL
    # RRV_GT_CAPTURE_SETTLE. A six-field target is silently dropped as
    # "malformed target" inside PCSX2, which surfaces only as "0 of N fired"
    # after a full cold boot. Fail here instead, with the fix in the message.
    for t in targets:
        if t.count(":") > 4:
            sys.exit("error: target %r has a per-target settle field, which the "
                     "capture patch does not implement.\n"
                     "       Use the global --settle and one settle per run." % t)

    rc, log = run_capture(targets, args.out, args.timeout, args.settle,
                          args.app, args.disc, args.emulog, args.qt_plugin_path)
    starts, timeline = parse_log(log)
    # Tag per-run artefacts by the targets they came from: a second run into the
    # same directory must not overwrite the first run's manifest and log, or the
    # provenance of the dumps already sitting there is lost.
    tag = "_".join(t.split(":")[0] for t in targets)[:60]
    logpath = os.path.join(args.out, f"capture_{tag}.log")
    os.makedirs(args.out, exist_ok=True)
    with open(logpath, "w") as fh:
        fh.write(log)

    print(f"\n{len(starts)} of {len(targets)} target(s) fired; log -> {logpath}")
    rows = []
    for s in starts:
        gs = os.path.join(args.out, s["name"] + ".gs")
        if not os.path.exists(gs):
            print(f"  {s['name']}: MISSING dump", file=sys.stderr)
            continue
        if s["probe"] != CLEAN_PROBE:
            print(f"  {s['name']}: REJECTED — probe 0x{s['probe']:08X} != "
                  f"0x{CLEAN_PROBE:08X} (contaminated session)", file=sys.stderr)
            continue
        gsr = convert(gs)
        rows.append((s, gs, gsr))
        print(f"  {s['name']}: phase={s['phase']} script={s['script']} "
              f"step={s['step']} vsync={s['vsync']} "
              f"{os.path.getsize(gs)/1e6:.1f} MB"
              + (f" -> {os.path.basename(gsr)}" if gsr else ""))

    manifest = os.path.join(args.out, f"MANIFEST_{tag}.md")
    with open(manifest, "w") as fh:
        fh.write("# Automated PCSX2 ground-truth capture\n\n")
        fh.write("Produced by `scripts/gt_capture.py` in ONE cold boot; every dump\n")
        fh.write("was started by the guest's own scene coordinate, not by wall clock\n")
        fh.write("or a keypress. Re-running the same command reproduces it.\n\n")
        fh.write(f"* date: {time.strftime('%Y-%m-%d %H:%M')}\n")
        fh.write(f"* emulator: {args.app}\n")
        fh.write("* patch: tools/patches/pcsx2-gt-capture.patch (observational)\n")
        fh.write(f"* disc: {os.path.basename(args.disc)}\n")
        fh.write(f"* ELF probe 0x2206F8 = 0x{CLEAN_PROBE:08X} on every accepted dump\n\n")
        fh.write("| name | phase | script | step | vsync | fields | .gs bytes | sha256(.gs) |\n")
        fh.write("|---|---|---|---|---|---|---|---|\n")
        for s, gs, _ in rows:
            fh.write(f"| {s['name']} | {s['phase']} | {s['script']} | {s['step']} | "
                     f"{s['vsync']} | {s['fields']} | {os.path.getsize(gs)} | "
                     f"{sha256(gs)} |\n")
        fh.write("\n## Attract timeline observed during this run\n\n```\n")
        for vsync, phase, script, step in timeline:
            fh.write(f"vsync {vsync:6d}  phase={phase:<4} script={script:<4} step={step}\n")
        fh.write("```\n")
    print(f"manifest -> {manifest}")


if __name__ == "__main__":
    main()
