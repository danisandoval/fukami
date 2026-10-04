#!/usr/bin/env python3
"""Asset-free tests for the statically recompiled VU microcode (RRV_VU_AOT).

Synthetic microcode only (no game data): the generator's block planner and
successor discovery, the emitted catalogue's shape, and the committed runtime's AOT
dispatch (third_party/ps2recomp).
"""
from __future__ import annotations

import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load('vu_aot_gen', ROOT / 'tools/vu-aot/vu_aot_gen.py')

NOP_LOWER = 0x8000033C
NOP_UPPER = 0x000002FF          # upper NOP (special 0x2F)
E_BIT = 0x40000000
I_BIT = 0x80000000


def b_to(pc: int, target: int) -> int:
    """B to target from a branch at pc: target = pc + 8 + imm11 * 8."""
    imm = ((target - pc - 8) // 8) & 0x7FF
    return (0x20 << 25) | imm


def ibne(pc: int, target: int) -> int:
    imm = ((target - pc - 8) // 8) & 0x7FF
    return (0x29 << 25) | (1 << 16) | (2 << 11) | imm


def image(slots: list[tuple[int, int]], size: int = 4096) -> bytes:
    data = bytearray(size)
    for i, (lower, upper) in enumerate(slots):
        struct.pack_into('<II', data, i * 8, lower, upper)
    return bytes(data)


class PlannerTests(unittest.TestCase):
    def test_block_ends_after_branch_delay_slot(self):
        img = image([(NOP_LOWER, NOP_UPPER), (b_to(8, 0x40), NOP_UPPER),
                     (NOP_LOWER, NOP_UPPER), (NOP_LOWER, NOP_UPPER)])
        slots, stop = gen.plan(img, 4096, 0)
        self.assertEqual([s['pc'] for s in slots], [0, 8, 16])
        self.assertFalse(stop)
        self.assertEqual(slots[2]['flags'], gen.MAYBE_BRANCH | gen.LAST)
        self.assertEqual(gen.successors(slots, stop, 4096), [0x40])

    def test_conditional_branch_follows_both_edges(self):
        img = image([(ibne(0, 0x80), NOP_UPPER), (NOP_LOWER, NOP_UPPER)])
        slots, stop = gen.plan(img, 4096, 0)
        self.assertEqual(sorted(gen.successors(slots, stop, 4096)), [16, 0x80])

    def test_e_bit_stops_one_slot_later(self):
        img = image([(NOP_LOWER, NOP_UPPER | E_BIT), (NOP_LOWER, NOP_UPPER),
                     (NOP_LOWER, NOP_UPPER)])
        slots, stop = gen.plan(img, 4096, 0)
        self.assertEqual(len(slots), 2)
        self.assertTrue(stop)
        self.assertEqual(slots[0]['flags'], gen.ARM_E)
        # The next program commonly starts after the delay slot.
        self.assertEqual(gen.successors(slots, stop, 4096), [16])

    def test_loi_lower_is_not_a_branch(self):
        # A float immediate whose bits look like a B opcode must not end a block.
        img = image([(b_to(0, 0x40), NOP_UPPER | I_BIT), (NOP_LOWER, NOP_UPPER | E_BIT),
                     (NOP_LOWER, NOP_UPPER)])
        slots, stop = gen.plan(img, 4096, 0)
        self.assertEqual(len(slots), 3)
        self.assertTrue(stop)

    def test_block_never_crosses_its_region_end(self):
        # At run time the last slot computes the wrapped fall-through itself.
        img = image([], 4096)
        slots, stop = gen.plan(img, 4096, 4096 - 8)
        self.assertEqual(len(slots), 1)
        self.assertFalse(stop)
        self.assertEqual(slots[0]['flags'], gen.LAST)

    def test_branch_targets_are_relative(self):
        # A branch at 8 to 0x40, planned from any region offset, targets +0x38.
        img = image([(NOP_LOWER, NOP_UPPER), (b_to(8, 0x40), NOP_UPPER), (NOP_LOWER, NOP_UPPER)])
        slots, stop = gen.plan(img, 4096, 8)
        self.assertEqual(gen.successors(slots, stop, 4096), [0x40])

    def test_length_cap(self):
        img = image([(NOP_LOWER, NOP_UPPER)] * 512)
        slots, _ = gen.plan(img, 4096, 0)
        self.assertEqual(len(slots), gen.MAX_SLOTS)

    def test_generated_catalogue_is_sorted_and_longest_first(self):
        img = image([(NOP_LOWER, NOP_UPPER), (b_to(8, 0), NOP_UPPER), (NOP_LOWER, NOP_UPPER),
                     (NOP_LOWER, NOP_UPPER | E_BIT), (NOP_LOWER, NOP_UPPER)])
        with tempfile.TemporaryDirectory() as tmp:
            rec = Path(tmp) / 'rec'
            rec.mkdir()
            (rec / 'vu0-04096-0000000000000001.bin').write_bytes(img)
            (rec / 'vu0-04096-0000000000000001.pcs').write_text('0000\n0008\n')
            out = Path(tmp) / 'out'
            sys.argv = ['vu_aot_gen', str(rec), '--output', str(out)]
            self.assertEqual(gen.main(), 0)
            text = (out / 'rrv_vu_aot_blocks.inc').read_text()
            self.assertIn('>(vu, p, pc);', text)          # position independent
            self.assertNotIn('0x0008u,', text.split('kVuAotBlocks')[0].split('VuAot::slot<')[1][:12])
            self.assertIn('kVuAotCatalogId', text)
            rows = [l for l in text.splitlines() if l.startswith('    {0x')]
            keys = [(int(r.split(',')[0].strip(' {').rstrip('ul'), 16), -int(r.split(',')[1].strip().rstrip('u')))
                    for r in rows]
            self.assertEqual(keys, sorted(keys))
            self.assertEqual(len(rows), len(set(r.split(', ')[3] for r in rows)))

    def test_elf_chain_extraction(self):
        # Two back-to-back MPG packets of synthetic microcode inside junk.
        prog = [(NOP_LOWER, NOP_UPPER)] * 3 + [(NOP_LOWER, NOP_UPPER | E_BIT), (NOP_LOWER, NOP_UPPER)]
        blob = bytearray(b'\x11' * 12)                       # junk, keeps o % 8 == 4
        for part, addr in ((prog[:2], 0x10), (prog[2:], 0x12)):
            blob += struct.pack('<II', 0, (0x4A << 24) | (len(part) << 16) | addr)[4:]
            blob[-8:-4] = b'\0\0\0\0'
            for lo, up in part:
                blob += struct.pack('<II', lo, up)
            blob += b'\0\0\0\0'
        chains = gen.elf_chains(bytes(blob) + b'\0' * 16)
        self.assertEqual(len(chains), 1)
        self.assertEqual(len(chains[0]), 5 * 8)


def up(op: int, dest: int = 0xF, ft: int = 0, fs: int = 0, fd: int = 0) -> int:
    """A primary upper op (bits 5:0)."""
    return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | op


def up2(op2: int, dest: int = 0xF, ft: int = 0, fs: int = 0) -> int:
    """An upper-special op: op2 = (upper & 3) | ((upper >> 4) & 0x7C)."""
    return (dest << 21) | (ft << 16) | (fs << 11) | ((op2 >> 2) << 6) | 0x3C | (op2 & 3)


ADD, MUL, MADD_X, MADD_W, MAX_X, MULA_X, ITOF0, FTOI0, OPMSUB = 0x28, 0x2A, 0x08, 0x0B, 0x10, 0x18, 0x10, 0x14, 0x2E
FMAND = 0x1A << 25
LQ = 0x00 << 25


def lq(it: int, dest: int = 0xF) -> int:
    return LQ | (dest << 21) | (it << 16) | (1 << 11)


def move(it: int, fs: int, dest: int = 0xF) -> int:
    return (0x40 << 25) | (dest << 21) | (it << 16) | (fs << 11) | (0x0C << 6) | 0x3C     # special2 0x30


def block(slots: list[tuple[int, int]]) -> list[dict]:
    planned, _ = gen.plan(image(slots + [(NOP_LOWER, NOP_UPPER | E_BIT)], 4096), len(slots) * 8, 0)
    assert len(planned) == len(slots)
    gen.analyse(planned)
    return planned


class AnalysisTests(unittest.TestCase):
    """Block analysis: which per-slot steps a compiled block leaves out. tests/vu_lean_diff_tests.cpp
    checks the result against the interpreter on every real block; these pin the rules on synthetic ones."""

    def test_snapshots_only_where_a_reader_or_the_next_block_sees_them(self):
        slots = block([(NOP_LOWER, NOP_UPPER)] * 9)
        for k, sl in enumerate(slots):
            self.assertEqual(bool(sl['flags'] & gen.NO_RING_WRITE), k < 5, k)   # the last four are kept
            self.assertTrue(sl['flags'] & gen.NO_VISIBLE)
        # A flag reader in slot 6 sees the snapshot of slot 2, and fetches it.
        slots = block([(NOP_LOWER, NOP_UPPER)] * 6 + [(FMAND | (1 << 16), NOP_UPPER)] + [(NOP_LOWER, NOP_UPPER)] * 2)
        self.assertEqual([k for k, sl in enumerate(slots) if not sl['flags'] & gen.NO_RING_WRITE], [2, 5, 6, 7, 8])
        self.assertEqual([k for k, sl in enumerate(slots) if not sl['flags'] & gen.NO_VISIBLE], [6])

    def test_a_short_block_keeps_every_snapshot(self):
        for sl in block([(NOP_LOWER, NOP_UPPER)] * 3):
            self.assertFalse(sl['flags'] & gen.NO_RING_WRITE)

    def test_an_immediate_is_not_a_flag_reader(self):
        # I bit: the lower word is a float whose bits look like FMAND.
        slots = block([(NOP_LOWER, NOP_UPPER)] * 4 + [(FMAND, NOP_UPPER | I_BIT)] + [(NOP_LOWER, NOP_UPPER)] * 4)
        self.assertTrue(slots[0]['flags'] & gen.NO_RING_WRITE)
        self.assertTrue(slots[4]['flags'] & gen.NO_VISIBLE)

    def test_mac_word_only_where_a_kept_snapshot_or_the_block_end_sees_it(self):
        fmac = (NOP_LOWER, up(ADD, fs=1, ft=2, fd=3))
        # FMAC ops in slots 0, 1, 2 of nine: 0 and 1 are overwritten before slot 5 pushes a snapshot.
        slots = block([fmac] * 3 + [(NOP_LOWER, NOP_UPPER)] * 6)
        self.assertEqual([bool(sl['flags'] & gen.NO_MAC_WORD) for sl in slots[:3]], [True, True, False])
        # A reader in slot 5 sees slot 1's snapshot, which holds slot 1's MAC word.
        slots = block([fmac] * 3 + [(NOP_LOWER, NOP_UPPER)] * 2 + [(FMAND | (1 << 16), NOP_UPPER)] +
                      [(NOP_LOWER, NOP_UPPER)] * 3)
        self.assertEqual([bool(sl['flags'] & gen.NO_MAC_WORD) for sl in slots[:3]], [True, False, False])
        # The last FMAC op of a block always leaves its word: it is the MAC register afterwards.
        slots = block([fmac] * 9)
        self.assertEqual([k for k, sl in enumerate(slots) if not sl['flags'] & gen.NO_MAC_WORD], [5, 6, 7, 8])
        # MAX is not an FMAC op: it has no MAC word to skip, and it does not end an FMAC op's reach.
        slots = block([fmac, (NOP_LOWER, up(MAX_X, fs=1, ft=2, fd=3))] + [(NOP_LOWER, NOP_UPPER)] * 7)
        self.assertFalse(slots[0]['flags'] & gen.NO_MAC_WORD)
        self.assertFalse(slots[1]['flags'] & gen.NO_MAC_WORD)

    def clean(self, slots, k):
        return slots[k]['flags'] & (gen.CLEAN_VS | gen.CLEAN_VT | gen.CLEAN_ACC)

    def test_operand_clamp_is_kept_for_anything_not_written_in_the_block(self):
        slots = block([(NOP_LOWER, NOP_UPPER), (NOP_LOWER, up(MADD_X, fs=1, ft=2, fd=3))])
        self.assertEqual(self.clean(slots, 1), 0)
        # An op that does not read an operand needs no clamp for it (ADD: no accumulator).
        slots = block([(NOP_LOWER, NOP_UPPER), (NOP_LOWER, up(ADD, fs=1, ft=2, fd=3))])
        self.assertEqual(self.clean(slots, 1), gen.CLEAN_ACC)

    def test_fmac_results_and_itof_need_no_clamp(self):
        slots = block([(NOP_LOWER, up2(MULA_X, fs=1, ft=2)),           # ACC = vf1 * vf2.x
                       (NOP_LOWER, up(MADD_X, fs=1, ft=2, fd=3)),      # vf3 = ACC + vf1 * vf2.x
                       (NOP_LOWER, up(ADD, fs=3, ft=3, fd=4)),         # vf4 = vf3 + vf3
                       (NOP_LOWER, up2(ITOF0, fs=9, ft=5)),            # vf5 = itof(vf9)
                       (NOP_LOWER, up(MUL, fs=5, ft=4, fd=6)),         # vf6 = vf5 * vf4
                       (NOP_LOWER, up2(FTOI0, fs=6, ft=6)),            # vf6 = ftoi(vf6): any bits
                       (NOP_LOWER, up(MUL, fs=6, ft=4, fd=7))])
        self.assertEqual(self.clean(slots, 1), gen.CLEAN_ACC)
        self.assertEqual(self.clean(slots, 2), gen.CLEAN_VS | gen.CLEAN_VT | gen.CLEAN_ACC)
        self.assertEqual(self.clean(slots, 4), gen.CLEAN_VS | gen.CLEAN_VT | gen.CLEAN_ACC)
        self.assertEqual(self.clean(slots, 6), gen.CLEAN_VT | gen.CLEAN_ACC)

    def test_only_the_lanes_an_op_wrote_and_an_op_reads(self):
        slots = block([(NOP_LOWER, up(ADD, dest=0xE, fs=1, ft=2, fd=3)),      # vf3.xyz
                       (NOP_LOWER, up(ADD, dest=0xF, fs=3, ft=1, fd=4)),      # reads vf3.xyzw
                       (NOP_LOWER, up(ADD, dest=0xC, fs=3, ft=1, fd=4)),      # reads vf3.xy
                       (NOP_LOWER, up(MADD_X, dest=0x1, fs=1, ft=3, fd=5)),   # reads vf3.x
                       (NOP_LOWER, up(MADD_W, dest=0x1, fs=1, ft=3, fd=5))])  # reads vf3.w
        self.assertFalse(slots[1]['flags'] & gen.CLEAN_VS)
        self.assertTrue(slots[2]['flags'] & gen.CLEAN_VS)
        self.assertTrue(slots[3]['flags'] & gen.CLEAN_VT)
        self.assertFalse(slots[4]['flags'] & gen.CLEAN_VT)

    def test_a_load_forgets_and_a_move_carries(self):
        fmac3 = (NOP_LOWER, up(ADD, fs=1, ft=2, fd=3))
        use3 = (NOP_LOWER, up(ADD, fs=3, ft=3, fd=4))
        self.assertTrue(block([fmac3, use3])[1]['flags'] & gen.CLEAN_VS)
        # LQ in the lower pipe of the slot that wrote vf3 runs after the upper op.
        self.assertFalse(block([(lq(3), fmac3[1]), use3])[1]['flags'] & gen.CLEAN_VS)
        slots = block([(move(8, 3), fmac3[1]), (NOP_LOWER, up(ADD, fs=8, ft=8, fd=4))])
        self.assertTrue(slots[1]['flags'] & gen.CLEAN_VS)
        slots = block([fmac3, (move(3, 9), NOP_UPPER), use3])                 # vf3 = vf9: unknown again
        self.assertFalse(slots[2]['flags'] & gen.CLEAN_VS)

    def test_vf0_is_known_only_after_the_first_slot(self):
        use0 = (NOP_LOWER, up(MADD_W, fs=1, ft=0, fd=3))                      # vt = vf0.w
        slots = block([use0, use0])
        self.assertFalse(slots[0]['flags'] & gen.CLEAN_VT)
        self.assertTrue(slots[1]['flags'] & gen.CLEAN_VT)

    def test_outer_product_reads_the_raw_accumulator_and_three_lanes(self):
        slots = block([(NOP_LOWER, up(ADD, dest=0xE, fs=1, ft=2, fd=3)),
                       (NOP_LOWER, up(OPMSUB, dest=0xE, fs=3, ft=3, fd=4)),
                       (NOP_LOWER, up(ADD, dest=0x8, fs=1, ft=2, fd=5)),
                       (NOP_LOWER, up(OPMSUB, dest=0x8, fs=5, ft=5, fd=4))])
        self.assertEqual(self.clean(slots, 1), gen.CLEAN_VS | gen.CLEAN_VT | gen.CLEAN_ACC)
        self.assertEqual(self.clean(slots, 3), gen.CLEAN_ACC)                 # x alone is not enough

    def test_an_unknown_lower_op_forgets_everything(self):
        fmac3 = up(ADD, fs=1, ft=2, fd=3)
        unknown = (0x40 << 25) | (0x1F << 6) | 0x3F                           # special2 0x7F: not in the tables
        slots = block([(unknown, fmac3), (NOP_LOWER, up(ADD, fs=3, ft=3, fd=4))])
        self.assertEqual(self.clean(slots, 1), gen.CLEAN_ACC)

    def test_decode_matches_the_runtime_tables(self):
        # rrv_vu_ir.h is the decoder the compiled blocks use; every op it marks UF_MAC / UF_FLUSH must be
        # marked the same here, or the engine's static_asserts reject the catalogue.
        ir = (ROOT / 'third_party/ps2recomp/ps2xRuntime/include/runtime/rrv_vu_ir.h').read_text()
        import re
        def table(name, size):
            body = ir.split(f'inline constexpr UpperEntry {name}[{size}] = {{')[1].split('};')[0]
            body = re.sub(r'//.*', '', body)
            entries = re.findall(r'(kMac|kPlain)\((U_\w+), (UD_\w+)\)|UpperEntry\{(U_\w+), (UD_\w+), (\w+)\}', body)
            out = []
            for kind, op, dst, op2, dst2, flags2 in entries:
                if kind:
                    out.append((op, dst, kind == 'kMac', kind == 'kMac'))
                else:
                    out.append((op2, dst2, 'UF_MAC' in flags2, 'UF_FLUSH' in flags2))
            return out
        dst_name = {'fd': 'UD_FD', 'ft': 'UD_FT', 'acc': 'UD_ACC', None: 'UD_NONE'}
        for name, size, mine in (('kUpper', 64, gen.UPPER), ('kUpperSpecial', 128, gen.UPPER_SPECIAL)):
            theirs = table(name, size)
            self.assertGreaterEqual(len(theirs), 48)
            for i, (op, dst, mac, flush) in enumerate(theirs):
                entry = mine[i]
                self.assertEqual((dst_name[entry[1]], entry[2], entry[3]), (dst, mac, flush), (name, hex(i), op))
            for entry in mine[len(theirs):]:
                self.assertEqual(entry[0], 'NOP')

    def test_no_analysis_emits_only_the_planner_flags(self):
        img = image([(NOP_LOWER, up(ADD, fs=1, ft=2, fd=3))] * 6 + [(NOP_LOWER, NOP_UPPER | E_BIT), (NOP_LOWER, NOP_UPPER)])
        with tempfile.TemporaryDirectory() as tmp:
            rec = Path(tmp) / 'rec'
            rec.mkdir()
            (rec / 'vu1-04096-0000000000000002.bin').write_bytes(img)
            ids = []
            for extra in ([], ['--no-analysis']):
                out = Path(tmp) / ('out' + str(len(extra)))
                sys.argv = ['vu_aot_gen', str(rec), '--output', str(out)] + extra
                self.assertEqual(gen.main(), 0)
                text = (out / 'rrv_vu_aot_blocks.inc').read_text()
                flags = [int(l.split(',')[3].split('u>')[0]) for l in text.splitlines() if 'VuAot::slot<' in l]
                ids.append(text.split('kVuAotCatalogId[] = "')[1][:16])
                if extra:
                    self.assertTrue(all(f < 8 for f in flags))
                else:
                    self.assertTrue(any(f & gen.NO_MAC_WORD for f in flags))
            self.assertNotEqual(ids[0], ids[1])       # the id names what the blocks compile to


class SourceTests(unittest.TestCase):
    """The committed runtime (third_party/ps2recomp) carries the AOT dispatch: no JIT."""
    RT = ROOT / 'third_party/ps2recomp/ps2xRuntime'

    def read(self, relative):
        return (self.RT / relative).read_text()

    def test_runtime_has_no_jit_and_templated_handlers(self):
        vu1 = self.read('src/lib/ps2_vu1.cpp')
        vu1h = self.read('include/runtime/ps2_vu1.h')
        ir = self.read('include/runtime/rrv_vu_ir.h')
        runtime = self.read('src/lib/ps2_runtime.cpp')
        engine = (ROOT / 'src/vu-aot/rrv_vu_aot_engine.inc').read_text()
        for gone in ('m_jit', 'jitEnsure', 'rrv_vu_jit', 'vujit'):
            self.assertNotIn(gone, vu1)
        self.assertNotIn('vujit', vu1h)
        self.assertIn('execUpperFastT<-1>(ir);', vu1)
        self.assertIn('execLowerT<-1, -1, -1>(instr', vu1)
        self.assertIn('constexpr bool kAotCfg = KOP >= 0;', vu1)
        self.assertTrue(vu1.rstrip().endswith('#include "rrv_vu_aot_engine.inc"'))
        self.assertIn('constexpr SlotIR decodeSlotValue', ir)
        # RRV_VU0_LEAN / RRV_VU1_LEAN: VCALLMS and MSCAL try the lean entry first; run() can resume
        # inside a delay slot (a per-instance flag: the units hand over on different threads).
        self.assertIn('if (!m_vu0.aotLean(', runtime)
        self.assertIn('if (!m_vu1.aotLean(', runtime)
        self.assertNotIn('int &count = seen[address];', runtime)
        self.assertIn('if (!m_aotLeanResume)', vu1)
        self.assertNotIn('s_aotVu0Resume', vu1 + vu1h + engine)
        self.assertIn('bool aotPrevBranch = m_state.branchPending;', vu1)
        self.assertIn('bool aotLean(', vu1h)
        self.assertIn('bool m_aotLeanResume = false;', vu1h)
        self.assertIn('bool VU1Interpreter::aotLean(', engine)
        self.assertNotIn('aotVu0Lean', vu1 + vu1h + engine + runtime)

if __name__ == '__main__':
    unittest.main()
