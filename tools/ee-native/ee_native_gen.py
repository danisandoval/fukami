#!/usr/bin/env python3
"""Native hot EE functions: exact batched-clock versions of generated functions.

Input: the accounted generation (``local/gate8/<gen>/accounted-generation``) the
product compiles. Output: ``generated/rr5/native/rrv_ee_native.inc`` plus a
manifest. The .inc is derivative of the game code: like ``../output/`` it is
committed only to the private repository (owner exception 2026-09-08).

WHAT CHANGES
  The generated code calls two runtime functions before every guest
  instruction: ``gate3CheckpointV1`` (noinline) and ``gate3BeginInstructionV1``.
  Inside the Gate-4 lazy window (see scripts/gate4_lazy_checkpoint_overlay.py)
  the checkpoint's only effect is ``*lazy.now = cycles``. A native version keeps
  every instruction statement of the generated function byte for byte and
  replaces the two calls with a local cycle counter:

    checkpoint  -> if cycle < window end and the quiet generation is unchanged,
                   remember it (fast); else commit and run the real checkpoint
    begin       -> ++cycle

  The counter, the context's instruction count and the owner clock are
  committed before every runtime interaction (MMIO load/store, VU0
  microprogram, exception, call to another guest function, return), so the
  runtime sees exactly the state the per-instruction calls would have left.
  Back-edge scheduler checks are skipped only while the window is armed and
  the generation is unchanged: then the scheduler is provably quiet and the
  check is a no-op (every scheduler change bumps the generation). Scratchpad
  loads and stores use the runtime's own SPR fast path inline.

  Nothing else changes: same statements, same order, same guest-visible state
  at every runtime interaction. A function containing anything the translator
  does not know (syscalls, COP0 writes, unknown runtime methods) is refused.

HOOKS (generator version 2)
  A hand-written native routine can take over a stretch of a native function
  between two of its labels (a label is a branch target or resume point of the
  generated function, so the guest state at a label is all in R5900Context):

    --hook FUNCTION:FROM:TO=callee
        at label FROM:  if (callee(rdram, ctx, runtime, rrvNative)) goto label_TO;
        The callee either leaves everything untouched and returns false (the
        generated statements then run, as without the hook), or leaves the
        context, memory and the clock exactly as the generated statements from
        FROM up to (not including) TO would have, and returns true.
    --probe FUNCTION:AT=callee
        at label AT:    callee(rdram, ctx, runtime, rrvNative);
        An observation point (the callee must not change guest state): used to
        check a hook against the generated statements it replaces.

  The callees are declared before the .inc is included (src/product). The
  manifest records every hook and probe; `--from-manifest` regenerates with the
  functions, hooks and probes of an existing manifest.

Usage:
  python3 tools/ee-native/ee_native_gen.py --generation generated/rr5 \
      --output generated/rr5/native --from-manifest generated/rr5/native/manifest.json
  python3 tools/ee-native/ee_native_gen.py --generation generated/rr5 \
      --output generated/rr5/native [--hook ...] [--probe ...] 0x222eb8 0x223f18 ...
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

GENERATOR_VERSION = 2

# runtime-> methods the translator understands. Anything else: refuse.
KNOWN_RT = {
    'gate3CheckpointV1', 'gate3BeginInstructionV1', 'shouldPreemptGuestExecution',
    'hasFunction', 'lookupFunction', 'SignalException', 'executeVU0Microprogram', 'handleBreak',
}
REFUSE = ('cop0_status', 'handleSyscall', 'ps2_syscalls::', 'ps2_stubs::',
          'handleTrap', 'handleTLB', 'raiseCop0Exception', 'gate3Charge')

FUNC_RE = re.compile(r'^void (sub_([0-9A-F]{8})_0x([0-9a-f]+))\(uint8_t\* rdram, R5900Context\* ctx, PS2Runtime \*runtime\) \{$')
WORD_RE = re.compile(r'^\s*// 0x([0-9a-f]+): 0x([0-9a-f]+)\s')
CALL_RE = re.compile(r'^(\s*)((?:targetFn|sub_[0-9A-F]{8}_0x[0-9a-f]+)\(rdram, ctx, runtime\);)$')
LABEL_RE = re.compile(r'^label_([0-9a-f]+):$')
CALLEE_RE = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*$')


def parse_hook(text: str, probe: bool) -> dict:
    """FUNCTION:FROM:TO=callee (hook) or FUNCTION:AT=callee (probe); addresses in hex."""
    try:
        where, callee = text.split('=', 1)
        parts = [int(x, 16) for x in where.split(':')]
    except ValueError:
        raise SystemExit(f'bad {"probe" if probe else "hook"}: {text!r}')
    if len(parts) != (2 if probe else 3) or not CALLEE_RE.match(callee):
        raise SystemExit(f'bad {"probe" if probe else "hook"}: {text!r}')
    entry = {'function': f'0x{parts[0]:x}', 'at': f'0x{parts[1]:x}', 'callee': callee}
    if not probe:
        entry['to'] = f'0x{parts[2]:x}'
    return entry


def translate(path: Path, hooks: list[dict] = (), probes: list[dict] = ()) -> tuple[str, str, list[tuple[int, int]]]:
    lines = path.read_text().split('\n')
    out: list[str] = []
    words: list[tuple[int, int]] = []
    name = None
    body = False
    labels = {m.group(1) for m in map(LABEL_RE.match, lines) if m}
    at = {}
    for h in hooks:
        for key in ('at', 'to'):
            if h[key][2:] not in labels:
                raise ValueError(f'{path.name}: hook {h["callee"]}: no label at {h[key]}')
        at.setdefault(h['at'][2:], []).append(
            f'    if ({h["callee"]}(rdram, ctx, runtime, rrvNative)) goto label_{h["to"][2:]}; // native hook')
    for h in probes:
        if h['at'][2:] not in labels:
            raise ValueError(f'{path.name}: probe {h["callee"]}: no label at {h["at"]}')
        at.setdefault(h['at'][2:], []).append(f'    {h["callee"]}(rdram, ctx, runtime, rrvNative); // native probe')
    for line in lines:
        m = FUNC_RE.match(line)
        if m:
            name = m.group(1)
            out.append(f'static void rrv_native_{name}(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {{')
            out.append('    rrv_native::Clock rrvNative(runtime, ctx);')
            body = True
            continue
        if not body:
            continue
        code = line.split('//', 1)[0] if not line.lstrip().startswith('//') else ''
        for bad in REFUSE:
            if bad in code:
                raise ValueError(f'{path.name}: refuses {bad!r}: {line.strip()}')
        for rt in re.findall(r'runtime->(\w+)', code):
            if rt not in KNOWN_RT:
                raise ValueError(f'{path.name}: unknown runtime method {rt}')
        w = WORD_RE.match(line)
        if w:
            words.append((int(w.group(1), 16), int(w.group(2), 16)))
        s = line.strip()
        if s == 'runtime->gate3CheckpointV1(ctx);':
            line = line.replace('runtime->gate3CheckpointV1(ctx);', 'rrvNative.ck();')
        elif s == 'runtime->gate3BeginInstructionV1(ctx);':
            line = line.replace('runtime->gate3BeginInstructionV1(ctx);', 'rrvNative.begin();')
        elif CALL_RE.match(line):
            ind, call = CALL_RE.match(line).groups()
            line = f'{ind}rrvNative.sync(); {call} rrvNative.reload();'
        else:
            line = line.replace('runtime->shouldPreemptGuestExecution()', 'rrvNative.preempt()')
            line = line.replace('runtime->SignalException(', 'rrvNative.signal(')
            line = line.replace('runtime->executeVU0Microprogram(', 'rrvNative.vu0(')
            line = line.replace('runtime->handleBreak(', 'rrvNative.brk(')
            if 'gate3CheckpointV1' in line or 'gate3BeginInstructionV1' in line:
                raise ValueError(f'{path.name}: unexpected accounting form: {line.strip()}')
        out.append(line)
        label = LABEL_RE.match(line)
        if label and label.group(1) in at:
            out.extend(at[label.group(1)])
    if not name:
        raise ValueError(f'{path.name}: no function')
    if not words:
        raise ValueError(f'{path.name}: no instruction words')
    return name, '\n'.join(out).rstrip() + '\n', words


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--generation', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--hook', action='append', default=[], metavar='FUNCTION:FROM:TO=callee')
    ap.add_argument('--probe', action='append', default=[], metavar='FUNCTION:AT=callee')
    ap.add_argument('--from-manifest', type=Path, help='functions, hooks and probes of this manifest')
    ap.add_argument('addresses', nargs='*')
    a = ap.parse_args()
    hooks = [parse_hook(h, False) for h in a.hook]
    probes = [parse_hook(h, True) for h in a.probe]
    if a.from_manifest:
        if a.addresses or hooks or probes:
            raise SystemExit('--from-manifest takes no addresses, hooks or probes')
        previous = json.loads(a.from_manifest.read_text())
        a.addresses = [f['address'] for f in previous['functions']]
        hooks, probes = previous.get('hooks', []), previous.get('probes', [])
    if not a.addresses:
        raise SystemExit('no function addresses')
    known = {f'0x{int(x, 16):x}' for x in a.addresses}
    for h in hooks + probes:
        if h['function'] not in known:
            raise SystemExit(f'hook or probe for {h["function"]}, which is not generated')
    gen_out = a.generation / 'output'
    manifest_path = a.generation / 'gate3-accounted-generation-manifest.json'
    parts = [
        '// Generated by tools/ee-native/ee_native_gen.py -- do not edit.\n'
        '// Derivative of the game code: private repository only (see README.md).\n',
    ]
    funcs = []
    for text in a.addresses:
        addr = int(text, 16)
        hits = sorted(gen_out.glob(f'sub_{addr:08X}_0x{addr:x}.cpp'))
        if len(hits) != 1:
            raise SystemExit(f'no unique generated source for 0x{addr:x}')
        src = hits[0]
        key = f'0x{addr:x}'
        name, code, words = translate(src, [h for h in hooks if h['function'] == key],
                                      [h for h in probes if h['function'] == key])
        table = ', '.join(f'{{0x{p:x}u, 0x{v:08x}u}}' for p, v in words)
        parts.append(f'// ---- {name} ({len(words)} instructions) from {src.name}\n')
        parts.append(f'static const rrv_native::Word rrv_native_words_{name}[] = {{{table}}};\n')
        parts.append(code)
        funcs.append({'address': f'0x{addr:x}', 'name': name, 'source': src.name,
                      'source_sha256': hashlib.sha256(src.read_bytes()).hexdigest(),
                      'instructions': len(words)})
    parts.append('static const rrv_native::Entry rrv_native_entries[] = {\n')
    for f in funcs:
        n = f['name']
        parts.append(f'    {{{f["address"]}u, "{n}", &{n}, &rrv_native_{n}, rrv_native_words_{n}, '
                     f'sizeof(rrv_native_words_{n}) / sizeof(rrv_native_words_{n}[0])}},\n')
    parts.append('};\n')
    inc = ''.join(parts)
    a.output.mkdir(parents=True, exist_ok=True)
    (a.output / 'rrv_ee_native.inc').write_text(inc)
    manifest = {
        'schema': 1,
        'generator': 'tools/ee-native/ee_native_gen.py',
        'generator_version': GENERATOR_VERSION,
        'generation_manifest_sha256': hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
        'functions': funcs,
        'hooks': hooks,
        'probes': probes,
        'inc_sha256': hashlib.sha256(inc.encode()).hexdigest(),
    }
    (a.output / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n')
    print(f'{len(funcs)} functions -> {a.output}/rrv_ee_native.inc ({len(inc)} bytes)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
