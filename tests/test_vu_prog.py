#!/usr/bin/env python3
"""Asset-free tests for the whole-program native VU microprograms (RRV_VU_PROG).

Synthetic microcode only (no game data): the generator's labels, segments and
terminators (tools/vu-aot/vu_prog_gen.py), the shapes it must hand over at, its
indirect-jump targets, its operand analysis over a join, the position
independence of the emitted text, and the committed catalogue against its
manifest. What the programs compute is tests/vu_prog_diff_tests.cpp.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load('vu_prog_gen', ROOT / 'tools/vu-aot/vu_prog_gen.py')

NOP = 0x8000033C                # lower NOP (MOVE.0 vf00, vf00)
UNOP = 0x000002FF               # upper NOP
E_BIT = 0x40000000
XGKICK = 0x800006FC | (11 << 11)  # XGKICK vi11 (lower special2 0x6C)


def rel(pc: int, target: int) -> int:
    return ((target - pc - 8) // 8) & 0x7FF


def b(pc: int, target: int) -> int:
    return (0x20 << 25) | rel(pc, target)


def bal(pc: int, target: int, link: int) -> int:
    return (0x21 << 25) | (link << 16) | rel(pc, target)


def jr(reg: int) -> int:
    return (0x24 << 25) | (reg << 11)


def ibne(pc: int, target: int, it: int = 1, isr: int = 2) -> int:
    return (0x29 << 25) | (it << 16) | (isr << 11) | rel(pc, target)


def iaddiu(it: int, isr: int, imm: int) -> int:
    return (0x08 << 25) | (it << 16) | (isr << 11) | (imm & 0x7FF) | ((imm & 0x7800) << 10)


def lq(it: int, isr: int, imm: int, dest: int = 0xF) -> int:
    return (0x00 << 25) | (dest << 21) | (it << 16) | (isr << 11) | (imm & 0x7FF)


def add(fd: int, fs: int, ft: int, dest: int = 0xF) -> int:
    return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | 0x28


def mfp(it: int) -> int:
    """A lower op the engine leaves to execLowerT (MFP, special2 0x64)."""
    return (0x40 << 25) | (0xF << 21) | (it << 16) | 0x3C | ((0x64 >> 2) << 6) | (0x64 & 3)


def image(slots: dict[int, tuple[int, int]], size: int) -> bytes:
    data = bytearray(size)
    for pc in range(0, size, 8):
        struct.pack_into('<II', data, pc, *slots.get(pc, (NOP, UNOP)))
    return bytes(data)


def program(slots: dict[int, tuple[int, int]], size: int, entry: int = 0) -> 'gen.Program':
    p = gen.Program(image(slots, size), entry)
    p.discover()
    p.build()
    p.low = p.all_slots()[0]
    return p


class ShapeTests(unittest.TestCase):
    def test_straight_line_to_the_e_bit(self):
        p = program({0x10: (NOP, UNOP | E_BIT)}, 0x40)
        self.assertEqual(sorted(p.labels), [0])
        seg = p.segments[0]
        self.assertEqual(seg['slots'], [0x00, 0x08])
        self.assertEqual(seg['term'], ('end', 0x10))
        # The E slot and its delay slot run, then the program ends.
        self.assertEqual([pc for pc, _ in p.executed(0)], [0x00, 0x08, 0x10, 0x18])

    def test_loop_with_a_conditional_branch(self):
        # 0x00 B 0x20 | 0x08 delay | 0x20 body | 0x28 IBNE 0x20 | 0x30 delay | 0x38 E | 0x40 delay
        p = program({0x00: (b(0x00, 0x20), UNOP), 0x28: (ibne(0x28, 0x20), UNOP),
                     0x38: (NOP, UNOP | E_BIT)}, 0x60)
        self.assertEqual(sorted(p.labels), [0x00, 0x20, 0x38])
        self.assertEqual(p.segments[0x00]['term'], ('b', 0x00, 0x20))
        self.assertEqual(p.segments[0x20]['slots'], [0x20])
        self.assertEqual(p.segments[0x20]['term'], ('cond', 0x28, 0x20, 0x38))
        self.assertEqual(p.segments[0x38]['term'], ('end', 0x38))
        # The branch's slot and its delay slot are charged with the segment.
        self.assertEqual([pc for pc, _ in p.executed(0x20)], [0x20, 0x28, 0x30])
        self.assertEqual(sorted(p.successors(0x20)), [0x20, 0x38])
        # The slots between the first delay slot and the loop are not part of the program.
        self.assertEqual(p.all_slots(), [0x00, 0x08, 0x20, 0x28, 0x30, 0x38, 0x40])

    def test_a_label_inside_a_straight_line_splits_it(self):
        # 0x10 is both fallen into from 0x08 and branched to from 0x18.
        p = program({0x18: (ibne(0x18, 0x10), UNOP), 0x28: (NOP, UNOP | E_BIT)}, 0x40)
        self.assertEqual(sorted(p.labels), [0x00, 0x10, 0x28])
        self.assertEqual(p.segments[0x00]['term'], ('fall', 0x10))
        self.assertEqual(p.segments[0x00]['slots'], [0x00, 0x08])

    def test_a_delay_slot_that_is_also_a_target_is_emitted_twice(self):
        # 0x00 IBNE 0x08: the delay slot 0x08 is the branch target as well.
        p = program({0x00: (ibne(0x00, 0x08), UNOP), 0x10: (NOP, UNOP | E_BIT)}, 0x30)
        self.assertIn(0x08, p.labels)
        self.assertEqual(p.executed(0x00), [(0x00, False), (0x08, True)])
        self.assertEqual(p.segments[0x08]['slots'], [0x08])


class HandOverTests(unittest.TestCase):
    def test_branch_in_a_delay_slot(self):
        p = program({0x08: (b(0x08, 0x20), UNOP), 0x10: (b(0x10, 0x28), UNOP)}, 0x40)
        self.assertEqual(p.segments[0]['slots'], [0x00])
        self.assertEqual(p.segments[0]['term'], ('bail', 0x08))

    def test_e_bit_on_a_branch_and_in_a_branch_delay_slot(self):
        p = program({0x08: (b(0x08, 0x20), UNOP | E_BIT)}, 0x40)
        self.assertEqual(p.segments[0]['term'], ('bail', 0x08))
        p = program({0x08: (b(0x08, 0x20), UNOP), 0x10: (NOP, UNOP | E_BIT)}, 0x40)
        self.assertEqual(p.segments[0]['term'], ('bail', 0x08))
        p = program({0x08: (NOP, UNOP | E_BIT), 0x10: (b(0x10, 0x20), UNOP)}, 0x40)
        self.assertEqual(p.segments[0]['term'], ('bail', 0x08))

    def test_target_and_fall_through_outside_the_image(self):
        p = program({0x08: (b(0x08, 0x80), UNOP)}, 0x40)
        self.assertEqual(p.segments[0]['term'], ('bail', 0x08))
        p = program({}, 0x20)                      # runs off the end
        self.assertEqual(p.segments[0]['slots'], [0x00, 0x08, 0x10, 0x18])
        self.assertEqual(p.segments[0]['term'], ('bail', 0x20))

    def test_the_emitted_function_hands_over_there(self):
        p = program({0x08: (b(0x08, 0x20), UNOP), 0x10: (b(0x10, 0x28), UNOP)}, 0x40)
        text = gen.emit_program(p, False)
        self.assertIn('bpc = base + 0x0008u;\n    goto L_out;', text)
        self.assertNotIn('slotBranch', text)


class JumpTests(unittest.TestCase):
    def test_call_and_return(self):
        # 0x00 BAL 0x20 (link vi15) | 0x08 delay | 0x10 E | 0x18 delay | 0x20 JR vi15 | 0x28 delay
        p = program({0x00: (bal(0x00, 0x20, 15), UNOP), 0x10: (NOP, UNOP | E_BIT),
                     0x20: (jr(15), UNOP)}, 0x40)
        self.assertEqual(sorted(p.labels), [0x00, 0x10, 0x20])
        self.assertEqual(p.segments[0x20]['term'], ('jump', 0x20, [0x10]))
        text = gen.emit_program(p, False)
        self.assertIn('case 0x0010u:\n        goto L_0010;', text)
        self.assertIn('default:\n        bpc = t;\n        goto L_out;', text)

    def test_jump_through_a_constant(self):
        # IADDIU vi01, vi00, 6 ; JR vi01 -> address 6 * 8 = 0x30
        p = program({0x00: (iaddiu(1, 0, 6), UNOP), 0x10: (jr(1), UNOP), 0x30: (NOP, UNOP | E_BIT)}, 0x50)
        self.assertEqual(p.segments[0]['term'], ('jump', 0x10, [0x30]))
        self.assertIn(0x30, p.labels)

    def test_jump_through_an_unknown_register_has_no_targets(self):
        p = program({0x00: (lq(1, 0, 4), UNOP), 0x08: (jr(2), UNOP)}, 0x40)
        self.assertEqual(p.segments[0]['term'], ('jump', 0x08, []))


class AnalysisTests(unittest.TestCase):
    def flags_at(self, p, label, pc):
        return p.flags[(label, pc, False)]

    def test_fmac_result_is_clean_and_a_load_is_not(self):
        # 0x00 ADD vf1 = vf2 + vf3 ; 0x08 ADD vf4 = vf1 + vf1 ; 0x10 LQ vf1 ; 0x18 ADD vf5 = vf1 + vf4
        p = program({0x00: (NOP, add(1, 2, 3)), 0x08: (NOP, add(4, 1, 1)), 0x10: (lq(1, 0, 4), UNOP),
                     0x18: (NOP, add(5, 1, 4)), 0x20: (NOP, UNOP | E_BIT)}, 0x40)
        p.analyse_clean()
        both = gen.CLEAN_VS | gen.CLEAN_VT
        self.assertEqual(self.flags_at(p, 0, 0x00) & both, 0)            # nothing known at the entry
        self.assertEqual(self.flags_at(p, 0, 0x08) & both, both)         # vf1 was just sanitised
        self.assertEqual(self.flags_at(p, 0, 0x18) & both, gen.CLEAN_VT)  # vf1 reloaded, vf4 still clean
        # ADD does not read the accumulator: that clamp is never needed.
        self.assertTrue(self.flags_at(p, 0, 0x00) & gen.CLEAN_ACC)

    def test_a_join_keeps_only_what_every_path_proves(self):
        # 0x00 IBNE 0x20 | 0x08 delay | 0x10 ADD vf1 = vf2 + vf3 (fall-through path only) | 0x18 NOP
        # 0x20 ADD vf4 = vf1 + vf1   <- reached with vf1 clean (fall-through) and unknown (branch)
        p = program({0x00: (ibne(0x00, 0x20), UNOP), 0x10: (NOP, add(1, 2, 3)), 0x20: (NOP, add(4, 1, 1)),
                     0x28: (NOP, UNOP | E_BIT)}, 0x40)
        p.analyse_clean()
        self.assertEqual(self.flags_at(p, 0x20, 0x20) & (gen.CLEAN_VS | gen.CLEAN_VT), 0)

    def test_a_loop_reaches_a_fixed_point(self):
        # 0x00 ADD vf1 = vf1 + vf2 ; 0x08 IBNE 0x00 ; 0x10 delay: LQ vf2
        # First pass: vf1 unknown. Around the loop vf1 is clean, vf2 is reloaded every time.
        p = program({0x00: (NOP, add(1, 1, 2)), 0x08: (ibne(0x08, 0x00), UNOP), 0x10: (lq(2, 0, 4), UNOP),
                     0x18: (NOP, UNOP | E_BIT)}, 0x30)
        p.analyse_clean()
        self.assertEqual(self.flags_at(p, 0, 0x00) & (gen.CLEAN_VS | gen.CLEAN_VT), 0)

    def test_lower_op_this_file_does_not_know_forgets_everything(self):
        p = program({0x00: (NOP, add(1, 2, 3)), 0x08: (0x7E000000, UNOP), 0x10: (NOP, add(4, 1, 1)),
                     0x18: (NOP, UNOP | E_BIT)}, 0x30)
        p.analyse_clean()
        self.assertEqual(self.flags_at(p, 0, 0x10) & (gen.CLEAN_VS | gen.CLEAN_VT), 0)


class EmitTests(unittest.TestCase):
    def test_addresses_are_relative_to_the_lowest_slot(self):
        slots = {0x40: (b(0x40, 0x60), UNOP), 0x68: (XGKICK, UNOP), 0x70: (NOP, UNOP | E_BIT)}
        p = program(slots, 0x90, entry=0x40)
        self.assertEqual(p.low, 0x40)
        text = gen.emit_program(p, False)
        self.assertIn('goto L_0000;', text)                     # the entry
        self.assertIn('L_0020:', text)                          # 0x60 - 0x40
        self.assertIn('base + 0x0028u', text)                   # the XGKICK's pc, for its packet tag
        self.assertIn('P::slotGeneric<', text)
        self.assertIn('bpc = P::nextPc(vu, base + 0x0038u);', text)

    def test_the_same_code_at_another_entry_is_the_same_text(self):
        one = program({0x00: (NOP, UNOP | E_BIT)}, 0x40, entry=0x00)
        two = program({0x10: (NOP, UNOP | E_BIT)}, 0x40, entry=0x10)
        self.assertEqual(gen.emit_program(one, False), gen.emit_program(two, False))

    def test_generic_and_native_lower_ops(self):
        self.assertTrue(gen.is_native(lq(1, 0, 4)))
        self.assertTrue(gen.is_native(iaddiu(1, 0, 6)))
        self.assertFalse(gen.is_native(XGKICK))
        self.assertFalse(gen.is_native(mfp(3)))
        p = program({0x00: (mfp(3), UNOP), 0x08: (NOP, UNOP | E_BIT)}, 0x20)
        self.assertEqual(gen.emit_program(p, False).count('P::slotGeneric<'), 1)

    def test_masks_name_every_register_and_register_zero(self):
        p = program({0x00: (lq(7, 9, 4), add(1, 2, 3)), 0x08: (NOP, UNOP | E_BIT)}, 0x20)
        vf, vi = p.masks()
        for reg in (0, 1, 2, 3, 7):
            self.assertTrue(vf >> reg & 1, reg)
        for reg in (0, 1, 9):
            self.assertTrue(vi >> reg & 1, reg)


class CatalogueTests(unittest.TestCase):
    """The committed catalogue (private repository; absent in a public checkout)."""

    def setUp(self):
        self.dir = ROOT / 'generated/rr5/vu'
        self.manifest = self.dir / 'programs.json'
        if not self.manifest.exists():
            self.skipTest('no program catalogue in this checkout')

    def test_manifest_names_the_committed_file_and_the_blocks_elf(self):
        manifest = json.loads(self.manifest.read_text())
        text = (self.dir / 'rrv_vu_aot_programs.inc').read_bytes()
        self.assertEqual(manifest['programs_inc_sha256'], hashlib.sha256(text).hexdigest())
        blocks = json.loads((self.dir / 'manifest.json').read_text())
        self.assertEqual(manifest['input_elf_sha256'], blocks['input_elf_sha256'])
        self.assertTrue(manifest['operand_analysis'])
        self.assertIn(f'kVuProgCatalogId[] = "{manifest["catalogue_id"]}"', text.decode())

    def test_every_listed_entry_is_a_program_or_names_one(self):
        manifest = json.loads(self.manifest.read_text())
        programs = {e['program'] for e in manifest['entries'] if 'program' in e}
        self.assertEqual(programs, set(range(manifest['programs'])))
        for e in manifest['entries']:
            if 'same_as_program' in e:
                self.assertIn(e['same_as_program'], programs)


if __name__ == '__main__':
    unittest.main()
