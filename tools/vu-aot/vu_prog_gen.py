#!/usr/bin/env python3
"""Generate whole-program native VU microprograms (RRV_VU_PROG).

Input (local/ only: it holds game microcode): --elf, the user's own dumped game
ELF. Every VIF MPG upload chain in it is extracted as tools/vu-aot/vu_aot_gen.py
does (one image per chain, addressed from 0).

Output: generated/rr5/vu/rrv_vu_aot_programs.inc (derivative code: private
repository only, like the compiled blocks beside it) and programs.json. The .inc
is included by src/vu-aot/rrv_vu_prog_engine.inc.

A PROGRAM is one C++ function for one entry of one image: every slot reachable
from the entry, in the function's own control flow, written as calls of
VU1Interpreter::Prog (the engine): slot<LOWER, UPPER, FLAGS>() and friends. The
engine keeps the registers in a local for the whole run; this file decides only
the shape:

  * Entries. The game starts a microprogram with MSCAL at a multiple of 0x10
    in the image's jump table, so every such offset below 0x100 is an entry.
    Any other start address simply has no program (the compiled blocks run).
  * Labels and segments. A label is the entry, a static branch target, the
    slot after a conditional branch's or a call's delay slot, or an expected
    indirect target. A segment runs from a label to the next label or to a
    terminator. Each segment charges its slots to the budget first; if the
    budget (maxCycles) would be exceeded the program hands over at the label,
    where no branch and no E bit is pending.
  * Branches. The decision is taken in the branch's slot, the delay slot is
    emitted right after it (again as a segment of its own if something else
    jumps to it), then the goto. Position independent: the image may be loaded
    at any address (`base`), branch targets are relative and a link register or
    an XGKICK tag gets base + offset.
  * E bit. The slot, its delay slot, then the end: pc is the wrapped
    fall-through, as VU1Interpreter::run() leaves it.
  * Indirect jumps (JR, JALR). Expected targets are the return points of the
    program's calls and, for a jump through a register loaded by IADDIU from
    VI0 in the same segment, that constant. Anything else hands over.
  * Hand-over (the compiled blocks continue at that pc with the exact state):
    a branch in a delay slot, an E bit on or right after a branch, a target or
    a fall-through outside the image, an unexpected indirect target.

Operand analysis (analyse_clean): the operand clamp replaces an exponent of 255
and nothing else. A lane written by an FMAC op (sanitised), by ITOF, by
MAXbc/MINIbc/ABS/MOVE/MR32 of such lanes, or belonging to VF0 cannot hold one.
The compiled blocks prove this inside one block (vu_aot_gen.py, analyse); here
it is a forward dataflow over the whole program: a lane is clean at a slot only
if it is clean on every path from the entry (nothing is known at the entry). The
transfer functions are the block generator's. FLAGS carries CLEAN_VS / CLEAN_VT /
CLEAN_ACC for an op whose every used lane of that operand is clean.
--no-analysis emits FLAGS = 0 everywhere (every clamp runs).
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path

_spec = importlib.util.spec_from_file_location('vu_aot_gen', Path(__file__).with_name('vu_aot_gen.py'))
blk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(blk)

ENTRY_LIMIT = 0x100
ENTRY_STEP = 0x10
CLEAN_VS, CLEAN_VT, CLEAN_ACC = 1, 2, 4      # VU1Interpreter::Prog::kClean*
ALL_LANES, XYZ, ACC = blk.ALL_LANES, blk.XYZ, blk.ACC

B, BAL, JR, JALR = 0x20, 0x21, 0x24, 0x25
COND_OPS = blk.COND_OPS


def has_lower(lower: int, upper: int) -> bool:
    return blk.has_lower(lower, upper)


def is_branch(lower: int, upper: int) -> bool:
    return blk.lower_is_branch(lower, upper)


def lower_op(lower: int) -> int:
    return (lower >> 25) & 0x7F


def is_native(lower: int) -> bool:
    """VU1Interpreter::Prog::isNative (rrv_vu_prog_engine.inc)."""
    op = lower_op(lower)
    if op in (0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
              0x18, 0x1A, 0x1B, 0x1C):
        return True
    if op != 0x40:
        return False
    funct = lower & 0x3F
    if funct in (0x30, 0x31, 0x32, 0x34, 0x35):
        return True
    if funct < 0x3C:
        return False
    f2 = (lower & 0x3) | ((lower >> 4) & 0x7C)
    return f2 in (0x30, 0x31, 0x3D, 0x3C, 0x3E, 0x3F, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B,
                  0x68, 0x69)


class Program:
    """One entry of one image: labels, segments and what they need."""

    def __init__(self, image: bytes, entry: int):
        self.image = image
        self.end = len(image)
        self.entry = entry
        self.labels: set[int] = set()
        self.segments: dict[int, dict] = {}
        self.flags: dict[tuple[int, int, bool], int] = {}
        self.low = 0

    def words(self, pc: int) -> tuple[int, int]:
        return struct.unpack_from('<II', self.image, pc)

    def inside(self, pc: int) -> bool:
        return 0 <= pc and pc + 8 <= self.end and pc % 8 == 0

    # ---- shape of one slot ----------------------------------------------------
    def classify(self, pc: int):
        """('slot',) | ('bail',) | ('end',) | ('branch', kind, target) for the slot at pc.

        'end': an E bit here; this slot and the next run, then the program ends.
        'branch': kind is 'b', 'cond' or 'jump'; the delay slot is pc + 8."""
        if not self.inside(pc):
            return ('bail',)
        lower, upper = self.words(pc)
        e_bit = bool(upper & 0x40000000)
        branch = is_branch(lower, upper)
        if e_bit:
            if branch or not self.inside(pc + 8):
                return ('bail',)
            dl, du = self.words(pc + 8)
            if is_branch(dl, du):
                return ('bail',)
            return ('end',)
        if not branch:
            return ('slot',)
        if not self.inside(pc + 8):
            return ('bail',)
        dl, du = self.words(pc + 8)
        if is_branch(dl, du) or (du & 0x40000000):
            return ('bail',)
        op = lower_op(lower)
        if op in (JR, JALR):
            return ('branch', 'jump', None)
        target = pc + 8 + blk.imm11(lower) * 8
        if not self.inside(target):
            return ('bail',)
        return ('branch', 'cond' if op in COND_OPS else 'b', target)

    def constant_jump(self, pc: int, start: int):
        """The target of a JR/JALR at pc whose register was loaded by
        `IADDIU reg, vi00, imm` earlier in the same straight line from `start`."""
        lower, _ = self.words(pc)
        reg = (lower >> 11) & 0xF
        if reg == 0:
            return None
        back = pc - 8
        while back >= start:
            bl, bu = self.words(back)
            if has_lower(bl, bu):
                op = lower_op(bl)
                it, isr, idr = (bl >> 16) & 0xF, (bl >> 11) & 0xF, (bl >> 6) & 0xF
                if op == 0x08 and it == reg and isr == 0:
                    imm = (bl & 0x7FF) | ((bl >> 10) & 0x7800)
                    return ((imm & 0xFFFF) * 8) & 0x3FFF
                # Anything else that may write an integer register ends the search.
                if not (op in (0x00, 0x01, 0x05) or (op in (0x04, 0x08, 0x09) and it != reg) or
                        (op == 0x40 and (bl & 0x3F) in (0x30, 0x31, 0x34, 0x35) and idr != reg)):
                    return None
            back -= 8
        return None

    # ---- labels -----------------------------------------------------------------
    def discover(self) -> None:
        labels = {self.entry}
        returns: set[int] = set()              # the slot a call comes back to
        jumps: dict[int, int] = {}             # pc of an indirect jump -> start of its straight line
        visited: set[int] = set()

        def walk(todo: list[int]) -> None:
            while todo:
                start = todo.pop()
                pc = start
                while pc not in visited:
                    kind = self.classify(pc)
                    if kind[0] in ('bail', 'end'):
                        break
                    visited.add(pc)
                    if kind[0] == 'slot':
                        pc += 8
                        continue
                    _, how, target = kind
                    lower, _ = self.words(pc)
                    op = lower_op(lower)
                    nxt = pc + 16
                    if how == 'jump':
                        jumps[pc] = start
                    else:
                        labels.add(target)
                        todo.append(target)
                    if op in (BAL, JALR) and self.inside(nxt):
                        returns.add(nxt)
                    if how == 'cond' and self.inside(nxt):
                        labels.add(nxt)
                        todo.append(nxt)
                    break

        walk([self.entry])
        # Indirect targets: the program's call returns, and constants. A new
        # target is a label whose code is walked too, which may add more.
        self.jump_targets: dict[int, list[int]] = {}
        while True:
            new: set[int] = set()
            for pc, start in jumps.items():
                cands = set(returns)
                # The straight line the jump sits in starts at the nearest label at or before it.
                line = max(l for l in labels if l <= pc) if any(l <= pc for l in labels) else start
                const = self.constant_jump(pc, max(line, start))
                if const is not None and self.inside(const):
                    cands.add(const)
                self.jump_targets[pc] = sorted(cands)
                new |= cands - labels
            if not new:
                break
            labels |= new
            walk(sorted(new))
        self.labels = labels

    # ---- segments ---------------------------------------------------------------
    def build(self) -> None:
        """Every label's straight line. A segment is
        {'slots': [pc...], 'term': (...)}; term is one of
          ('fall', next_label) ('bail', pc) ('end', pc)
          ('b', pc, target) ('cond', pc, target, next) ('jump', pc, [targets])."""
        for label in sorted(self.labels):
            slots: list[int] = []
            pc = label
            while True:
                if pc != label and pc in self.labels:
                    term = ('fall', pc)
                    break
                kind = self.classify(pc)
                if kind[0] == 'bail':
                    term = ('bail', pc)
                    break
                if kind[0] == 'end':
                    term = ('end', pc)
                    break
                if kind[0] == 'slot':
                    slots.append(pc)
                    pc += 8
                    continue
                _, how, target = kind
                if how == 'jump':
                    term = ('jump', pc, self.jump_targets.get(pc, []))
                elif how == 'cond':
                    nxt = pc + 16
                    term = ('cond', pc, target, nxt if nxt in self.labels else None)
                else:
                    term = ('b', pc, target)
                break
            self.segments[label] = {'slots': slots, 'term': term}

    def executed(self, label: int) -> list[tuple[int, bool]]:
        """(pc, is_delay_copy) of every slot the segment runs, in order."""
        seg = self.segments[label]
        out = [(pc, False) for pc in seg['slots']]
        term = seg['term']
        if term[0] in ('b', 'cond', 'jump', 'end'):
            out.append((term[1], False))
            out.append((term[1] + 8, True))
        return out

    def successors(self, label: int) -> list[int]:
        term = self.segments[label]['term']
        if term[0] == 'fall':
            return [term[1]]
        if term[0] == 'b':
            return [term[2]]
        if term[0] == 'cond':
            return [term[2]] + ([term[3]] if term[3] is not None else [])
        if term[0] == 'jump':
            return list(term[2])
        return []

    def all_slots(self) -> list[int]:
        pcs = set()
        for label in self.segments:
            pcs.update(pc for pc, _ in self.executed(label))
        return sorted(pcs)

    def masks(self) -> tuple[int, int]:
        """The registers the program names (VF mask, VI mask); register 0 always."""
        vf = vi = 1
        for pc in self.all_slots():
            lower, upper = self.words(pc)
            vf |= 1 << ((upper >> 16) & 0x1F) | 1 << ((upper >> 11) & 0x1F) | 1 << ((upper >> 6) & 0x1F)
            if has_lower(lower, upper):
                vf |= 1 << ((lower >> 16) & 0x1F) | 1 << ((lower >> 11) & 0x1F)
                vi |= 1 << ((lower >> 16) & 0xF) | 1 << ((lower >> 11) & 0xF) | 1 << ((lower >> 6) & 0xF)
                vi |= 1 << 1                    # the flag readers' VI01
        return vf, vi

    # ---- operand analysis -------------------------------------------------------
    def transfer(self, pc: int, clean: list[int]) -> tuple[int, int, int]:
        """Runs one slot over the clean-lane table; returns (FLAGS, clamps, clamps skipped)."""
        lower, upper = self.words(pc)
        (name, dst, mac, flush, vt_mode, uses_acc), dest, ft, fs, fd, bc = blk.decode_upper(upper)
        flags = used = skipped = 0
        if flush:
            vs_need = XYZ if vt_mode == 'op' else dest
            vt_need = {'bc': 8 >> bc, 'full': dest, 'op': XYZ, None: None}[vt_mode]
            acc_need = dest if uses_acc else None
            for need, reg, bit in ((vs_need, fs, CLEAN_VS), (vt_need, ft, CLEAN_VT), (acc_need, ACC, CLEAN_ACC)):
                if need is None:
                    flags |= bit                 # the op does not read this operand
                    continue
                used += 1
                if clean[reg] & need == need:
                    flags |= bit
                    skipped += 1
        if dst is not None:
            target = {'fd': fd, 'ft': ft, 'acc': ACC}[dst]
            if mac or name == 'ITOF':
                new = dest                       # sanitised / converted: never exponent 255
            elif name in ('MAXbc', 'MINIbc'):    # one of its two operands, lane by lane
                new = (clean[fs] & dest) if clean[ft] & (8 >> bc) else 0
            elif name == 'ABS':                  # the exponent is the operand's
                new = clean[fs] & dest
            else:                                # FTOI, MAX, MINI, MAXi, MINIi: unknown
                new = 0
            clean[target] = (clean[target] & ~dest) | new
        if has_lower(lower, upper) and not is_branch(lower, upper):
            blk.lower_transfer(lower, clean)
        clean[0] = ALL_LANES                     # every slot ends with VF0 = (0, 0, 0, 1)
        return flags, used, skipped

    def analyse_clean(self) -> tuple[int, int]:
        """Forward dataflow: the clean lanes at every label (meet = AND over the
        predecessors, nothing known at the entry), then FLAGS per executed slot.
        A delay-slot copy is analysed where it is emitted. Returns (clamps, skipped)."""
        state: dict[int, list[int] | None] = {label: None for label in self.segments}
        state[self.entry] = [0] * 33
        work = [self.entry]
        while work:
            label = work.pop()
            clean = list(state[label])
            for pc, _ in self.executed(label):
                self.transfer(pc, clean)
            for succ in self.successors(label):
                old = state[succ]
                new = clean if old is None else [a & b for a, b in zip(old, clean)]
                if old is None or new != old:
                    state[succ] = list(new)
                    work.append(succ)
        clamps = skipped = 0
        for label in self.segments:
            clean = list(state[label]) if state[label] is not None else [0] * 33
            for pc, delay in self.executed(label):
                flags, used, skip = self.transfer(pc, clean)
                self.flags[(label, pc, delay)] = flags
                clamps += used
                skipped += skip
        return clamps, skipped


def emit_slot(p: Program, label: int, pc: int, delay: bool, analysis: bool, vf: int, vi: int) -> str:
    lower, upper = p.words(pc)
    flags = p.flags.get((label, pc, delay), 0) if analysis else 0
    args = f'0x{lower:08x}u, 0x{upper:08x}u, {flags}u'
    if has_lower(lower, upper) and not is_native(lower):
        return f'    P::slotGeneric<{args}, 0x{vf:08x}u, 0x{vi:04x}u>(vu, r, base + 0x{pc - p.low:04x}u);\n'
    return f'    P::slot<{args}>(r, vuData, dm);\n'


def emit_program(p: Program, analysis: bool) -> str:
    """The function's text with NAME and INDEX left as placeholders (identical
    programs are emitted once). Every address is relative to p.low, the lowest
    slot the program reaches: `base` is where that slot is loaded."""
    vf, vi = p.masks()
    low = p.low
    out = ['static uint32_t NAME(VU1Interpreter &vu, uint8_t *const vuData, const uint32_t dm, '
           'const uint32_t base, const uint32_t maxSlots, uint32_t &ended)\n{\n',
           '    using P = VU1Interpreter::Prog;\n    P::Regs r;\n',
           f'    P::enter<0x{vf:08x}u, 0x{vi:04x}u>(r, vu);\n',
           '    uint32_t n = 0u;\n    uint32_t bpc = 0u;\n    bool fin = false;\n'
           '    bool c = false;\n    uint32_t t = 0u;\n    (void)c;\n    (void)t;\n',
           f'    goto L_{p.entry - low:04x};\n']
    for label in sorted(p.segments):
        seg = p.segments[label]
        term = seg['term']
        ran = p.executed(label)
        out.append(f'L_{label - low:04x}:\n    RRV_VU_PROG_SEG(INDEX, 0x{label - low:04x}u);\n')
        if ran:
            out.append(f'    if (RRV_VU_UNLIKELY(n + {len(ran)}u > maxSlots))\n    {{\n'
                       f'        bpc = base + 0x{label - low:04x}u;\n        goto L_out;\n    }}\n'
                       f'    n += {len(ran)}u;\n')
        for pc in seg['slots']:
            out.append(emit_slot(p, label, pc, False, analysis, vf, vi))
        kind = term[0]
        if kind == 'fall':
            out.append(f'    goto L_{term[1] - low:04x};\n')
        elif kind == 'bail':
            out.append(f'    bpc = base + 0x{term[1] - low:04x}u;\n    goto L_out;\n')
        elif kind == 'end':
            pc = term[1]
            out.append(emit_slot(p, label, pc, False, analysis, vf, vi))
            out.append(emit_slot(p, label, pc + 8, True, analysis, vf, vi))
            out.append(f'    bpc = P::nextPc(vu, base + 0x{pc + 8 - low:04x}u);\n    fin = true;\n    goto L_out;\n')
        else:
            pc = term[1]
            lower, upper = p.words(pc)
            flags = p.flags.get((label, pc, False), 0) if analysis else 0
            args = f'0x{lower:08x}u, 0x{upper:08x}u, {flags}u'
            if kind == 'jump':
                out.append(f'    t = P::slotJump<{args}>(r, base + 0x{pc - low:04x}u);\n')
            else:
                out.append(f'    c = P::slotBranch<{args}>(r, base + 0x{pc - low:04x}u);\n')
            out.append(emit_slot(p, label, pc + 8, True, analysis, vf, vi))
            if kind == 'b':
                out.append(f'    goto L_{term[2] - low:04x};\n')
            elif kind == 'cond':
                out.append(f'    if (c)\n        goto L_{term[2] - low:04x};\n')
                if term[3] is not None:
                    out.append(f'    goto L_{term[3] - low:04x};\n')
                else:
                    out.append(f'    bpc = base + 0x{pc + 16 - low:04x}u;\n    goto L_out;\n')
            else:
                out.append('    switch (t - base)\n    {\n')
                for target in term[2]:
                    out.append(f'    case 0x{target - low:04x}u:\n        goto L_{target - low:04x};\n')
                out.append('    default:\n        bpc = t;\n        goto L_out;\n    }\n')
    out.append(f'L_out:\n    P::leave<0x{vf:08x}u, 0x{vi:04x}u>(vu, r, bpc, fin);\n'
               '    ended = fin ? 1u : 0u;\n    return n;\n}\n\n')
    return ''.join(out)


def ranges_of(pcs: list[int]) -> list[tuple[int, int]]:
    out: list[list[int]] = []
    for pc in pcs:
        if out and out[-1][0] + out[-1][1] * 8 == pc:
            out[-1][1] += 1
        else:
            out.append([pc, 1])
    return [(a, b) for a, b in out]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', type=Path, required=True, help='your own dumped game ELF')
    ap.add_argument('--output', type=Path, required=True, help='directory for rrv_vu_aot_programs.inc')
    ap.add_argument('--no-analysis', action='store_true', help='emit FLAGS = 0: every operand clamp runs')
    args = ap.parse_args()
    analysis = not args.no_analysis

    elf = args.elf.read_bytes()
    chains = blk.elf_chains(elf)
    out = ['// GENERATED by tools/vu-aot/vu_prog_gen.py — do not edit.\n',
           '// Whole-program native VU microprograms for RRV_VU_PROG (Ridge Racer V).\n',
           "// Derivative of the game's microcode: private repository only.\n\n",
           '// A test may count the segments a run enters (tests/vu_prog_diff_tests.cpp).\n'
           '#ifndef RRV_VU_PROG_SEG\n#define RRV_VU_PROG_SEG(program, label)\n#endif\n\n']
    table = []
    listing = []
    stats = {'vu0_images': 0, 'entries': 0, 'programs': 0, 'slots': 0, 'segments': 0, 'generic_slots': 0, 'hand_over_points': 0,
             'clamps': 0, 'clamps_skipped': 0}
    emitted: dict[tuple, int] = {}       # identical programs (stubs, shared code) are emitted once
    for ci, image in enumerate(chains):
        n = len(image) // 8
        # XGKICK exists on VU1 only: an image without one is a VU0 image (counted; its programs are
        # generated like any other: VCALLMS enters at the same multiples of 0x10).
        def is_xgkick(pc: int) -> bool:
            lower, upper = struct.unpack_from('<II', image, pc)
            return (has_lower(lower, upper) and lower_op(lower) == 0x40 and (lower & 0x3F) >= 0x3C and
                    ((lower & 0x3) | ((lower >> 4) & 0x7C)) == 0x6C)
        if not any(is_xgkick(pc) for pc in range(0, len(image), 8)):
            stats['vu0_images'] += 1
        out.append(f'static const uint64_t vuprog_image_{ci}[] = {{')
        out.append(', '.join(f'0x{w:016x}ull' for w in struct.unpack_from(f'<{n}Q', image)))
        out.append('};\n\n')
        for entry in range(0, min(ENTRY_LIMIT, len(image)), ENTRY_STEP):
            p = Program(image, entry)
            if p.classify(entry)[0] == 'bail':
                continue
            p.discover()
            p.build()
            pcs = p.all_slots()
            p.low = pcs[0]
            clamps = skipped = 0
            if analysis:
                clamps, skipped = p.analyse_clean()
            body = emit_program(p, analysis)
            rng = ranges_of([pc - p.low for pc in pcs])
            key = (body, tuple(rng), entry - p.low, tuple(struct.unpack_from('<Q', image, pc)[0] for pc in pcs))
            stats['entries'] += 1
            if key in emitted:
                listing.append({'image': ci, 'entry': entry, 'same_as_program': emitted[key]})
                continue
            index = len(table)
            emitted[key] = index
            name = f'vuprog_{index:05d}'
            out.append(f'// image {ci}, entry 0x{entry:04x}, lowest slot 0x{p.low:04x}\n')
            out.append(body.replace('NAME', name).replace('INDEX', f'{index}u'))
            out.append(f'static const VU1Interpreter::ProgRange {name}_ranges[] = {{')
            out.append(', '.join(f'{{0x{a:04x}u, {b}u}}' for a, b in rng))
            out.append('};\n\n')
            first = struct.unpack_from('<Q', image, entry)[0]
            table.append(f'    {{0x{entry - p.low:04x}u, 0x{first:016x}ull, vuprog_image_{ci} + {p.low // 8}, '
                         f'{n - p.low // 8}u, {name}_ranges, {len(rng)}u, {index}u, {len(p.segments)}u, &{name}}},\n')
            generic = sum(1 for pc in pcs if has_lower(*p.words(pc)) and not is_native(p.words(pc)[0])
                          and not is_branch(*p.words(pc)))
            bails = sum(1 for seg in p.segments.values() if seg['term'][0] in ('bail', 'jump') or
                        (seg['term'][0] == 'cond' and seg['term'][3] is None))
            stats['programs'] += 1
            stats['slots'] += len(pcs)
            stats['segments'] += len(p.segments)
            stats['generic_slots'] += generic
            stats['hand_over_points'] += bails
            stats['clamps'] += clamps
            stats['clamps_skipped'] += skipped
            listing.append({'image': ci, 'entry': entry, 'program': index, 'slots': len(pcs),
                            'segments': len(p.segments), 'generic_slots': generic, 'hand_over_points': bails})
    digest = hashlib.sha256(''.join(out + table).encode()).hexdigest()
    out.append('const VU1Interpreter::ProgEntry kVuProgs[] = {\n')
    out.extend(table)
    out.append('};\n')
    out.append('constexpr size_t kVuProgCount = sizeof(kVuProgs) / sizeof(kVuProgs[0]);\n')
    out.append(f'constexpr const char kVuProgCatalogId[] = "{digest[:16]}";\n')

    args.output.mkdir(parents=True, exist_ok=True)
    text = ''.join(out)
    (args.output / 'rrv_vu_aot_programs.inc').write_text(text)
    manifest = {
        'schema': 'rrv-vu-prog-catalogue-v1',
        'catalogue_id': digest[:16],
        'generator': 'tools/vu-aot/vu_prog_gen.py',
        'elf_chains': len(chains),
        'operand_analysis': analysis,
        **stats,
        'input_elf_sha256': hashlib.sha256(elf).hexdigest(),
        'programs_inc_sha256': hashlib.sha256(text.encode()).hexdigest(),
        'entries': listing,
    }
    (args.output / 'programs.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({k: v for k, v in manifest.items() if k != 'entries'}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
