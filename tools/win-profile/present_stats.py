#!/usr/bin/env python3
"""How evenly the frames of one Windows run reached the screen, from a present trace.

  python tools/win-profile/present_stats.py TRACE [--skip 600] [--json FILE]

TRACE is the file RRV_PCSX2_GS_PRESENT_TRACE=<path> writes at exit (RRVPresentPacer.h in
tools/patches/pcsx2-gs-bridge-windows-d3d11.patch): one line per present,
"arrival ready slot call returned present_count present_refresh sync_refresh sync_qpc", times in seconds.

Prints:
- call: the interval between present calls (what the renderer controls): median, p5, p95, and the share of
  intervals more than 1 ms and more than 2 ms off the 16.683 ms period.
- waits: how long the GS thread waited for the GPU to finish the frame and then for its slot, and how long the
  present call itself took.
- on screen (Direct3D 11 only: DXGI frame statistics): for how many display refreshes each frame stayed up,
  the refresh period, and the share of frames that are not one of the two counts nearest the ideal
  (ideal = refresh rate / 59.94: 4.00 at 240 Hz, 2.75 at 165 Hz, 2.00 at 120 Hz). With a multiple of 60 Hz every
  frame can have the same count; at 165 Hz the best possible is a regular 3-3-3-2.
- beat (the same data): wrong counts per --bucket seconds of the run. The game makes 59.94 frames a second and a
  60, 120 or 240 Hz display shows 60.00, so about every 16.7 s one frame must stay up a refresh longer, and while
  the frames arrive near the refresh edge their jitter adds short and long ones around it. Wrong counts that come
  in clumps at that spacing are this beat; wrong counts spread over every bucket are jitter.
Vulkan has no display feedback on Windows: its on-screen columns are absent and only the call cadence is known.

--skip drops the first presents (boot, logos, shader compiles). Host-only: it reads a file.
"""
from __future__ import annotations

import argparse
import collections
import json
from pathlib import Path
import statistics

PERIOD = 1001 / 60000


def pct(values, q):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(len(ordered) * q))]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('trace', type=Path)
    parser.add_argument('--skip', type=int, default=600, help='presents to drop at the start (default 600, 10 s)')
    parser.add_argument('--bucket', type=float, default=5.0, help='seconds per bucket of the beat view (default 5)')
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()

    rows = []
    for line in args.trace.read_text(encoding='ascii').splitlines():
        p = line.split()
        if len(p) == 9:  # arrival ready slot call returned present_count present_refresh sync_refresh sync_qpc
            rows.append((float(p[0]), float(p[2]), float(p[3]), float(p[4]), int(p[5]), int(p[6]), int(p[7]), float(p[8]),
                         float(p[1])))
    rows = rows[args.skip:]
    if len(rows) < 120:
        raise SystemExit(f'{args.trace}: only {len(rows)} presents after --skip')
    out: dict = {'presents': len(rows), 'paced': sum(1 for r in rows if r[1] > 0) / len(rows)}

    calls = [b[2] - a[2] for a, b in zip(rows, rows[1:])]
    out['call_ms'] = {'median': statistics.median(calls) * 1e3, 'p5': pct(calls, .05) * 1e3, 'p95': pct(calls, .95) * 1e3,
                      'off_1ms': sum(1 for c in calls if abs(c - PERIOD) > 1e-3) / len(calls),
                      'off_2ms': sum(1 for c in calls if abs(c - PERIOD) > 2e-3) / len(calls)}
    gpu = [max(0.0, r[8] - r[0]) for r in rows]
    waits = [max(0.0, r[2] - r[8]) for r in rows]
    blocks = [r[3] - r[2] for r in rows if r[3] > 0]
    out['gpu_wait_ms'] = {'mean': statistics.mean(gpu) * 1e3, 'p95': pct(gpu, .95) * 1e3, 'max': max(gpu) * 1e3}
    out['wait_ms'] = {'mean': statistics.mean(waits) * 1e3, 'p95': pct(waits, .95) * 1e3, 'max': max(waits) * 1e3}
    if blocks:
        out['block_ms'] = {'mean': statistics.mean(blocks) * 1e3, 'p95': pct(blocks, .95) * 1e3, 'max': max(blocks) * 1e3}

    # DXGI frame statistics: present number -> the refresh it was first shown at.
    shown: dict[int, int] = {}
    sync = []
    for r in rows:
        if r[4] and r[5]:
            shown.setdefault(r[4], r[5])
        if r[6] and r[7] > 0:
            sync.append((r[6], r[7]))
    if len(shown) > 60 and len(sync) > 60:
        (r0, t0), (r1, t1) = sync[0], sync[-1]
        refresh = (t1 - t0) / (r1 - r0) if r1 > r0 else 0.0
        counts = [shown[p + 1] - shown[p] for p in sorted(shown) if p + 1 in shown]
        ideal = PERIOD / refresh if refresh else 0.0
        near = {int(ideal), int(ideal) + 1} if abs(ideal - round(ideal)) > 0.02 else {round(ideal)}
        histogram = collections.Counter(counts)
        out['on_screen'] = {'refresh_hz': 1 / refresh if refresh else 0.0, 'ideal_refreshes': ideal, 'frames': len(counts),
                            'histogram': {str(k): histogram[k] for k in sorted(histogram)},
                            'wrong': sum(1 for c in counts if c not in near) / len(counts),
                            'missing_feedback': 1 - len(counts) / (len(rows) - 1)}
        # The beat: where in the run the wrong counts are. A present's time is that of its call.
        called = {r[4]: r[2] for r in reversed(rows) if r[4]}
        start = rows[0][2]
        buckets: dict[int, list[int]] = collections.defaultdict(lambda: [0, 0])
        for p in sorted(shown):
            if p + 1 in shown and p in called:
                bucket = buckets[int((called[p] - start) / args.bucket)]
                bucket[0] += 1
                bucket[1] += shown[p + 1] - shown[p] not in near
        whole = round(ideal)
        drift = abs(1 / (whole * refresh) - 1 / PERIOD) if refresh and whole and len(near) == 1 else 0.0
        out['beat'] = {'bucket_s': args.bucket, 'expected_period_s': 1 / drift if drift > 1e-6 else 0.0,
                       'wrong': [buckets[b][1] if b in buckets else 0 for b in range(max(buckets) + 1)] if buckets else [],
                       'frames': [buckets[b][0] if b in buckets else 0 for b in range(max(buckets) + 1)] if buckets else []}

    # Time on screen, which also holds on a variable-refresh display (where a count of refreshes says nothing): the
    # samples give the time of some vertical blanks; a present whose own blank was sampled has a known display time.
    blank_time = {r[6]: r[7] for r in rows if r[6] and r[7] > 0}
    if len(blank_time) > 60:
        ordered = sorted(blank_time)
        steps = [(blank_time[b] - blank_time[a]) / (b - a) for a, b in zip(ordered, ordered[1:]) if b - a <= 8]
        if steps:
            out['refresh_ms'] = {'median': statistics.median(steps) * 1e3, 'p5': pct(steps, .05) * 1e3, 'p95': pct(steps, .95) * 1e3}
        times = {p: blank_time[shown[p]] for p in shown if shown[p] in blank_time}
        held = [times[p + 1] - times[p] for p in sorted(times) if p + 1 in times]
        if len(held) > 60:
            out['held_ms'] = {'frames': len(held), 'median': statistics.median(held) * 1e3, 'p5': pct(held, .05) * 1e3,
                              'p95': pct(held, .95) * 1e3,
                              'off_1ms': sum(1 for h in held if abs(h - PERIOD) > 1e-3) / len(held),
                              'off_3ms': sum(1 for h in held if abs(h - PERIOD) > 3e-3) / len(held)}

    c = out['call_ms']
    print(f"presents {out['presents']} (paced {out['paced']:.0%})")
    print(f"call interval ms: median {c['median']:.2f} p5 {c['p5']:.2f} p95 {c['p95']:.2f}; more than 1 ms off the period "
          f"{c['off_1ms']:.1%}, more than 2 ms {c['off_2ms']:.1%}")
    w, g = out['wait_ms'], out['gpu_wait_ms']
    print(f"GS thread waits ms: for the GPU mean {g['mean']:.2f} p95 {g['p95']:.2f} max {g['max']:.1f}; "
          f"for the slot mean {w['mean']:.2f} p95 {w['p95']:.2f} max {w['max']:.1f}" +
          (f"; present call ms: mean {out['block_ms']['mean']:.2f} p95 {out['block_ms']['p95']:.2f} max {out['block_ms']['max']:.1f}"
           if blocks else ''))
    if 'on_screen' in out:
        s = out['on_screen']
        total = s['frames']
        hist = ' '.join(f"{k}:{v / total:.1%}" for k, v in s['histogram'].items())
        print(f"on screen: display {s['refresh_hz']:.2f} Hz, ideal {s['ideal_refreshes']:.2f} refreshes per frame; refreshes per frame {hist}")
        print(f"on screen: frames held for a wrong number of refreshes {s['wrong']:.1%} of {total} "
              f"(no feedback for {s['missing_feedback']:.1%} of presents)")
        b = out['beat']
        expected = (f"; this display and the game's 59.94 Hz meet every {b['expected_period_s']:.1f} s"
                    if b['expected_period_s'] else '')
        print(f"beat: wrong counts per {b['bucket_s']:g} s: {' '.join(str(w) for w in b['wrong'])}{expected}")
    else:
        print('on screen: no display feedback in this trace (Vulkan, or frame statistics unavailable)')
    if 'refresh_ms' in out:
        r = out['refresh_ms']
        print(f"display refresh interval ms: median {r['median']:.2f} p5 {r['p5']:.2f} p95 {r['p95']:.2f} "
              '(a spread means the display follows the presents: variable refresh)')
    if 'held_ms' in out:
        h = out['held_ms']
        print(f"time on screen ms ({h['frames']} frames with a sampled blank): median {h['median']:.2f} p5 {h['p5']:.2f} "
              f"p95 {h['p95']:.2f}; more than 1 ms off the period {h['off_1ms']:.1%}, more than 3 ms {h['off_3ms']:.1%}")
    if args.json:
        args.json.write_bytes((json.dumps(out, indent=1) + '\n').encode('utf-8'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
