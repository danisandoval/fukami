#!/usr/bin/env python3
"""Pacing and CPU of one game session, from the files the product writes at exit.

  python tools/win-profile/session_stats.py SESSION_DIR [--hitch-ms 25] [--phases] [--json FILE]

Reads start-clock.txt (host ns at each effective VBlank start), start-cpu.txt (CPU ns of the game thread at each
start), start-proc-cpu.txt (CPU ns of the whole process) and the `[gate3-observe] start=N ... phase=P` lines of
stderr.log. Prints, per stretch of the attract workload and in total: starts per second, the median, p95, p99 and
longest start interval, how many intervals exceed 1.25 and 1.9 periods, and CPU ms per start (game thread, process).
Then the long intervals with their start number and phase: the same start numbers in every run are guest work or
loading; start numbers that move, or that only the first run with a graphics API shows, are host hitches (shader
compiles). Host-only: it reads logs, nothing here touches a run.

Not Windows-specific (the files are the same on every platform); it lives here because the Windows port had no
`sample`/`perf` loop to lean on.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import statistics

PERIOD_MS = 1001 / 60000 * 1e3
# Stretches of the attract workload, by start number (docs/TESTING.md; the race is 4700 onward).
STRETCHES = [('logos 22-1070', 22, 1070), ('attract 1070-2329', 1070, 2329), ('attract 3D 2329-3287', 2329, 3287),
             ('race intro 3287-4700', 3287, 4700), ('race 4700-9900', 4700, 9900), ('after 9900', 9900, 10 ** 9)]


def load(path: Path) -> dict[int, int]:
    out: dict[int, int] = {}
    if path.is_file():
        for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0].isdigit():
                out[int(parts[0])] = int(parts[1])
    return out


def row(label: str, lo: int, hi: int, clock, cpu, proc) -> dict | None:
    hi = min(hi, max(clock))
    intervals = [(clock[i + 1] - clock[i]) / 1e6 for i in range(lo, hi) if i in clock and i + 1 in clock]
    if len(intervals) < 30:
        return None
    ordered = sorted(intervals)
    n = len(intervals)
    def per_start(series):
        return round((series[hi] - series[lo]) / n / 1e6, 2) if lo in series and hi in series and series[hi] else None
    return {'range': label, 'starts': n, 'starts_per_s': round(1000 / statistics.mean(intervals), 2),
            'median_ms': round(statistics.median(intervals), 2), 'p95_ms': round(ordered[int(n * .95)], 2),
            'p99_ms': round(ordered[int(n * .99)], 2), 'max_ms': round(ordered[-1], 1),
            'over_1.25': sum(1 for x in intervals if x > PERIOD_MS * 1.25),
            'over_1.9': sum(1 for x in intervals if x > PERIOD_MS * 1.9),
            'game_cpu_ms': per_start(cpu), 'process_cpu_ms': per_start(proc)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('session', type=Path)
    parser.add_argument('--hitch-ms', type=float, default=25.0, help='list start intervals longer than this')
    parser.add_argument('--phases', action='store_true', help='one row per scene phase instead of per stretch')
    parser.add_argument('--json', type=Path, help='also write the rows and hitches as JSON')
    args = parser.parse_args()

    clock = load(args.session / 'start-clock.txt')
    if len(clock) < 60:
        raise SystemExit(f'{args.session}: no start clock (the files are written when the game exits)')
    cpu, proc = load(args.session / 'start-cpu.txt'), load(args.session / 'start-proc-cpu.txt')
    log = (args.session / 'stderr.log').read_text(encoding='utf-8', errors='replace')
    phases = [(int(m.group(1)), int(m.group(2))) for m in re.finditer(r'\[gate3-observe\] start=(\d+) .*?phase=(\d+)', log)]
    first, last = min(clock), max(clock)

    ranges = [('all', first, last)]
    if args.phases:
        bounds = phases + [(last, -1)]
        ranges += [(f'phase {p} [{a},{b})', a, b) for (a, p), (b, _) in zip(bounds, bounds[1:])]
    else:
        ranges += STRETCHES
    rows = [r for r in (row(label, lo, hi, clock, cpu, proc) for label, lo, hi in ranges if lo < last) if r]
    for line in re.findall(r'^\[(?:pcsx2-gs-bridge\] (?:instantiated|presentation vsync)|gate3\] pacer waits).*$', log, re.M):
        print(line)
    header = ('range', 'starts', 'starts/s', 'median', 'p95', 'p99', 'max', '>20.9ms', '>31.7ms', 'game cpu', 'proc cpu')
    print('{:>24} {:>6} {:>8} {:>7} {:>6} {:>6} {:>7} {:>7} {:>7} {:>8} {:>8}'.format(*header))
    for r in rows:
        print('{:>24} {:>6} {:>8} {:>7} {:>6} {:>6} {:>7} {:>7} {:>7} {:>8} {:>8}'.format(
            r['range'], r['starts'], r['starts_per_s'], r['median_ms'], r['p95_ms'], r['p99_ms'], r['max_ms'],
            r['over_1.25'], r['over_1.9'], r['game_cpu_ms'] if r['game_cpu_ms'] is not None else '-',
            r['process_cpu_ms'] if r['process_cpu_ms'] is not None else '-'))

    def phase_of(start: int) -> int:
        current = -1
        for begin, phase in phases:
            if begin <= start:
                current = phase
        return current
    hitches = [{'start': i, 'phase': phase_of(i), 'ms': round((clock[i + 1] - clock[i]) / 1e6)}
               for i in sorted(clock) if i + 1 in clock and (clock[i + 1] - clock[i]) / 1e6 > args.hitch_ms]
    print(f'intervals over {args.hitch_ms:g} ms: {len(hitches)}')
    print(' '.join(f"{h['start']}(p{h['phase']}):{h['ms']}" for h in hitches))
    if args.json:
        args.json.write_bytes((json.dumps({'rows': rows, 'hitches': hitches}, indent=1) + '\n').encode('utf-8'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
