#!/usr/bin/env python3
"""Generate the statically recompiled VU microcode catalogue (RRV_VU_AOT).

Inputs (both local/ only: they hold game microcode):
  * --elf: the user's own dumped game ELF. Every VIF MPG upload chain in it is
    extracted statically (VIF NOP + MPG num,addr followed by the code, packets
    back to back), so every microprogram the game ships is compiled whether or
    not a recorded session ran it.
  * record directories written by a runtime run with RRV_VU_AOT_RECORD=<dir>:
    each program image is `vu<unit>-<codeSize>-<fnv64>.bin` (the whole micro
    memory at the time of a miss) with the missed pcs in the matching `.pcs`.
    They add the entry points and compositions seen at run time.

Blocks are POSITION INDEPENDENT: the game uploads the same microprogram at
different addresses (measured: an MPG chain whose header says address 0x200
runs at 0x440), and every VU branch is pc-relative, so a block is keyed by its
instruction words alone and receives its pc at run time.

Output: generated/rr5/vu/rrv_vu_aot_blocks.inc (derivative code: private repo
only, like generated/rr5/output) and a manifest. The .inc is included by the
staged ps2_vu1.cpp through src/vu-aot/rrv_vu_aot_engine.inc.

Blocks are discovered per region (a recorded image, or an extracted chain,
addressed from 0) from EVERY slot of an extracted chain (any entry the EE
may compute), and for a recorded image from every recorded pc, the start, the slot after
every E-bit delay slot (where the next program usually starts), every static
branch target, every fall-through after a conditional branch or a call, and
every call return; each discovered block's own successors are followed the
same way. A block never extends past the end of its region. The block planner is the one the ARM64 JIT used and the one
Aot::slot's flags assume:
  * a taken branch transfers control after its delay slot, so a block ends
    after the delay slot of any branch (the pc is a run-time select);
  * an E bit ends the program one slot later, so that slot is the block's
    stop slot;
  * blocks are capped at MAX_SLOTS (the dispatcher continues at the next pc).
Identical word sequences are emitted once.

Block analysis (analyse()): what each slot may leave out, proved from the
block's own words only, so it holds wherever the block runs and whatever runs
before and after it. The engine (src/vu-aot/rrv_vu_aot_engine.inc) does the
full per-slot sequence for any bit that is not set.
  * Flag visibility. A lower-pipe flag reader in slot k sees the snapshot
    {mac, clip, status} pushed by slot k-4. A snapshot is pushed only where a
    reader of this block will see it, or where it is still in the 4-entry ring
    when the block ends (its last four slots): NO_RING_WRITE otherwise. A slot
    without a reader does not fetch its snapshot: NO_VISIBLE.
  * MAC word. An FMAC op's MAC word is computed only if a pushed snapshot
    captures it or it is the block's last (it is the MAC register the block
    leaves behind): NO_MAC_WORD otherwise. The value the op stores is sanitised
    either way.
  * Operand clamp. The clamp replaces an exponent of 255 and nothing else.
    A lane written earlier in the block by an FMAC op (sanitised), by ITOF, by
    MAXbc/MINIbc/ABS/MOVE/MR32 of such lanes, or belonging to VF0, cannot hold one
    (rules this game's microcode never uses are left out: they could not be tested):
    CLEAN_VS / CLEAN_VT / CLEAN_ACC when every lane the op uses is such a lane.
    At block entry nothing is known (VF0 from the end of slot 0 on); a lower
    op this table does not know forgets everything.
--no-analysis emits the blocks without any of it (every step in every slot).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

MAX_SLOTS = 256
BRANCH_OPS = {0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}
STATIC_TARGET_OPS = {0x20, 0x21, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}
CALL_OPS = {0x21, 0x25}          # BAL, JALR: the return lands at branch pc + 16
COND_OPS = {0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}

MAYBE_BRANCH, ARM_E, LAST = 1, 2, 4
# Block analysis results; the values are Aot::kNoMacWord.. in rrv_vu_aot_engine.inc.
NO_MAC_WORD, NO_RING_WRITE, NO_VISIBLE, CLEAN_VS, CLEAN_VT, CLEAN_ACC = 8, 16, 32, 64, 128, 256

# ---- upper-pipe decode: rrv_vu_ir.h (kUpper / kUpperSpecial), as far as the
# analysis needs it. Each entry: (name, dst, mac, flush, vt, acc)
#   dst   'fd' | 'ft' | 'acc' | None
#   mac   the op is an FMAC op (UF_MAC): MAC word + sanitised result
#   flush the op clamps its operands (UF_FLUSH)
#   vt    how vf[ft] is read: 'bc' (one lane), 'full', 'op' (outer product), None
#   acc   the op reads ACC through the clamped copy
def _fmac(name, dst, vt, acc=False):
    return (name, dst, True, True, vt, acc)


def _plain(name, dst, vt):
    return (name, dst, False, False, vt, False)


_NOP = ('NOP', None, False, False, None, False)
UPPER = [_NOP] * 64
UPPER_SPECIAL = [_NOP] * 128
for _i in range(4):
    UPPER[0x00 + _i] = _fmac('ADDbc', 'fd', 'bc')
    UPPER[0x04 + _i] = _fmac('SUBbc', 'fd', 'bc')
    UPPER[0x08 + _i] = _fmac('MADDbc', 'fd', 'bc', True)
    UPPER[0x0C + _i] = _fmac('MSUBbc', 'fd', 'bc', True)
    UPPER[0x10 + _i] = _plain('MAXbc', 'fd', 'bc')
    UPPER[0x14 + _i] = _plain('MINIbc', 'fd', 'bc')
    UPPER[0x18 + _i] = _fmac('MULbc', 'fd', 'bc')
    UPPER_SPECIAL[0x00 + _i] = _fmac('ADDAbc', 'acc', 'bc')
    UPPER_SPECIAL[0x04 + _i] = _fmac('SUBAbc', 'acc', 'bc')
    UPPER_SPECIAL[0x08 + _i] = _fmac('MADDAbc', 'acc', 'bc', True)
    UPPER_SPECIAL[0x0C + _i] = _fmac('MSUBAbc', 'acc', 'bc', True)
    UPPER_SPECIAL[0x10 + _i] = _plain('ITOF', 'ft', None)
    UPPER_SPECIAL[0x14 + _i] = _plain('FTOI', 'ft', None)
    UPPER_SPECIAL[0x18 + _i] = _fmac('MULAbc', 'acc', 'bc')
UPPER[0x1C] = _fmac('MULq', 'fd', None)
UPPER[0x1D] = _plain('MAXi', 'fd', None)
UPPER[0x1E] = _fmac('MULi', 'fd', None)
UPPER[0x1F] = _plain('MINIi', 'fd', None)
UPPER[0x20] = _fmac('ADDq', 'fd', None)
UPPER[0x21] = _fmac('MADDq', 'fd', None, True)
UPPER[0x22] = _fmac('ADDi', 'fd', None)
UPPER[0x23] = _fmac('MADDi', 'fd', None, True)
UPPER[0x24] = _fmac('SUBq', 'fd', None)
UPPER[0x25] = _fmac('MSUBq', 'fd', None, True)
UPPER[0x26] = _fmac('SUBi', 'fd', None)
UPPER[0x27] = _fmac('MSUBi', 'fd', None, True)
UPPER[0x28] = _fmac('ADD', 'fd', 'full')
UPPER[0x29] = _fmac('MADD', 'fd', 'full', True)
UPPER[0x2A] = _fmac('MUL', 'fd', 'full')
UPPER[0x2B] = _plain('MAX', 'fd', 'full')
UPPER[0x2C] = _fmac('SUB', 'fd', 'full')
UPPER[0x2D] = _fmac('MSUB', 'fd', 'full', True)
UPPER[0x2E] = _fmac('OPMSUB', 'fd', 'op')        # reads the RAW accumulator
UPPER[0x2F] = _plain('MINI', 'fd', 'full')
UPPER_SPECIAL[0x1C] = _fmac('MULAq', 'acc', None)
UPPER_SPECIAL[0x1D] = _plain('ABS', 'ft', None)
UPPER_SPECIAL[0x1E] = _fmac('MULAi', 'acc', None)
UPPER_SPECIAL[0x1F] = _plain('CLIP', None, None)
UPPER_SPECIAL[0x20] = _fmac('ADDAq', 'acc', None)
UPPER_SPECIAL[0x21] = _fmac('MADDAq', 'acc', None, True)
UPPER_SPECIAL[0x22] = _fmac('ADDAi', 'acc', None)
UPPER_SPECIAL[0x23] = _fmac('MADDAi', 'acc', None, True)
UPPER_SPECIAL[0x24] = _fmac('SUBAq', 'acc', None)
UPPER_SPECIAL[0x25] = _fmac('MSUBAq', 'acc', None, True)
UPPER_SPECIAL[0x26] = _fmac('SUBAi', 'acc', None)
UPPER_SPECIAL[0x27] = _fmac('MSUBAi', 'acc', None, True)
UPPER_SPECIAL[0x28] = _fmac('ADDA', 'acc', 'full')
UPPER_SPECIAL[0x29] = _fmac('MADDA', 'acc', 'full', True)
UPPER_SPECIAL[0x2A] = _fmac('MULA', 'acc', 'full')
UPPER_SPECIAL[0x2C] = _fmac('SUBA', 'acc', 'full')
UPPER_SPECIAL[0x2D] = _fmac('MSUBA', 'acc', 'full', True)
UPPER_SPECIAL[0x2E] = ('OPMULA', 'acc', True, False, 'op', False)   # MAC, no operand clamp


def decode_upper(upper: int):
    op = upper & 0x3F
    if op >= 0x3C:
        entry = UPPER_SPECIAL[((upper & 0x3) | ((upper >> 4) & 0x7C)) & 0x7F]
    else:
        entry = UPPER[op]
    return entry, (upper >> 21) & 0xF, (upper >> 16) & 0x1F, (upper >> 11) & 0x1F, (upper >> 6) & 0x1F, upper & 3


# ---- lower pipe: execLowerT in ps2_vu1.cpp.
# Flag readers through the visibility ring (FCEQ FCAND FCOR FSEQ FSAND FSOR FMEQ FMAND FMOR FCGET).
FLAG_READERS = {0x10, 0x12, 0x13, 0x14, 0x16, 0x17, 0x18, 0x1A, 0x1B, 0x1C}
# Primary ops that write no VF register.
LOWER_NO_VF = {0x01, 0x04, 0x05, 0x08, 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
               0x1A, 0x1B, 0x1C, 0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}
# Lower-special functs (bits 5:0) that write no VF register.
SPECIAL_NO_VF = {0x30, 0x31, 0x32, 0x34, 0x35}
# Lower-special2 ops ((lower & 3) | ((lower >> 4) & 0x7C)) that write no VF register.
SPECIAL2_NO_VF = {0x3C, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x35, 0x37, 0x38, 0x39, 0x3A, 0x3B,
                  0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E,
                  0x68, 0x69, 0x6C}
# Lower-special2 ops that write vf[it] under the dest mask with a value of unknown exponent.
SPECIAL2_VF_UNKNOWN = {0x3D, 0x34, 0x36, 0x64}       # MFIR LQI LQD MFP

ALL_LANES = 0xF          # x = 8, y = 4, z = 2, w = 1 (the dest field's order)
XYZ = 0xE
ACC = 32                 # index of the accumulator in the clean-lane table


def has_lower(lower: int, upper: int) -> bool:
    """The slot runs a lower op: not an immediate (I bit), not a NOP."""
    return not (upper & 0x80000000) and lower not in (0x00000000, 0x8000033C)


def reads_flags(lower: int, upper: int) -> bool:
    return has_lower(lower, upper) and ((lower >> 25) & 0x7F) in FLAG_READERS


def lower_transfer(lower: int, clean: list[int]) -> None:
    """The lower op's effect on the clean-lane table (run after the upper op)."""
    op = (lower >> 25) & 0x7F
    dest = (lower >> 21) & 0xF
    it = (lower >> 16) & 0x1F
    fs = (lower >> 11) & 0x1F
    if op == 0x00:                                   # LQ
        clean[it] &= ~dest
        return
    if op in LOWER_NO_VF:
        return
    if op == 0x40:
        funct = lower & 0x3F
        if funct in SPECIAL_NO_VF:
            return
        if funct >= 0x3C:
            f2 = (lower & 0x3) | ((lower >> 4) & 0x7C)
            if f2 in SPECIAL2_NO_VF:
                return
            if f2 in SPECIAL2_VF_UNKNOWN:
                clean[it] &= ~dest
                return
            if f2 == 0x30:                           # MOVE: lane for lane
                clean[it] = (clean[it] & ~dest) | (clean[fs] & dest)
                return
            if f2 == 0x31:                           # MR32 rotates the lanes: all or nothing
                src = dest if clean[fs] == ALL_LANES else 0
                clean[it] = (clean[it] & ~dest) | src
                return
    # Not in the tables above: assume it may write anything.
    for r in range(len(clean)):
        clean[r] = 0


def analyse(slots) -> dict:
    """Adds the block-analysis bits to every slot's flags (module docstring) and
    returns what was left out, for the manifest."""
    n = len(slots)
    reader = [reads_flags(sl['lower'], sl['upper']) for sl in slots]
    # A snapshot pushed by slot k is read by slot k + 4, or outlives the block.
    ring_write = [k + 4 >= n or reader[k + 4] for k in range(n)]
    fmac = [k for k, sl in enumerate(slots) if decode_upper(sl['upper'])[0][2]]
    mac_word = set()
    for i, k in enumerate(fmac):
        until = fmac[i + 1] if i + 1 < len(fmac) else n      # the next MAC writer
        if until == n or any(ring_write[k:until]):
            mac_word.add(k)

    stats = {'slots': n, 'fmac': len(fmac), 'no_mac_word': 0, 'no_ring_write': 0, 'no_visible': 0,
             'clamps': 0, 'clamps_skipped': 0}
    # Nothing is known at entry, VF0 included: the engine re-asserts VF0 at the
    # end of slot 0 whatever the slot does, so slot 0 itself must not rely on it.
    clean = [0] * 33
    for k, sl in enumerate(slots):
        (name, dst, mac, flush, vt_mode, uses_acc), dest, ft, fs, fd, bc = decode_upper(sl['upper'])
        flags = 0
        if not reader[k]:
            flags |= NO_VISIBLE
            stats['no_visible'] += 1
        if not ring_write[k]:
            flags |= NO_RING_WRITE
            stats['no_ring_write'] += 1
        if mac and k not in mac_word:
            flags |= NO_MAC_WORD
            stats['no_mac_word'] += 1
        if flush:
            # The lanes of each operand that reach a destination lane.
            vs_need = XYZ if vt_mode == 'op' else dest
            vt_need = {'bc': 8 >> bc, 'full': dest, 'op': XYZ, None: None}[vt_mode]
            acc_need = dest if uses_acc else None
            for need, reg, bit in ((vs_need, fs, CLEAN_VS), (vt_need, ft, CLEAN_VT), (acc_need, ACC, CLEAN_ACC)):
                if need is None:
                    flags |= bit                     # the op does not read this operand
                    continue
                stats['clamps'] += 1
                if clean[reg] & need == need:
                    flags |= bit
                    stats['clamps_skipped'] += 1
        sl['flags'] |= flags

        # The upper op's write (it runs before the lower op of the same slot).
        if dst is not None:
            target = {'fd': fd, 'ft': ft, 'acc': ACC}[dst]
            if mac or name == 'ITOF':
                new = dest                           # sanitised / converted: never exponent 255
            elif name in ('MAXbc', 'MINIbc'):        # one of its two operands, lane by lane
                new = (clean[fs] & dest) if clean[ft] & (8 >> bc) else 0
            elif name == 'ABS':                      # the exponent is the operand's
                new = clean[fs] & dest
            else:                                    # FTOI, MAX, MINI, MAXi, MINIi: unknown
                new = 0
            clean[target] = (clean[target] & ~dest) | new
        if has_lower(sl['lower'], sl['upper']):
            lower_transfer(sl['lower'], clean)
        clean[0] = ALL_LANES                         # every slot ends with VF0 = (0, 0, 0, 1)
    assert not slots[-1]['flags'] & NO_RING_WRITE
    return stats


def words(image: bytes, pc: int) -> tuple[int, int]:
    return struct.unpack_from('<II', image, pc)


def lower_is_branch(lower: int, upper: int) -> bool:
    if upper & 0x80000000:          # LOI: the lower word is a float immediate
        return False
    if lower in (0x00000000, 0x8000033C):
        return False
    return ((lower >> 25) & 0x7F) in BRANCH_OPS


def imm11(lower: int) -> int:
    v = lower & 0x7FF
    return v - 0x800 if v & 0x400 else v


def plan(image: bytes, region_end: int, entry: int):
    """The slots of the block at `entry`, the JIT planner's rules, never past
    region_end (at run time the dispatcher only runs a block that fits below
    codeSize, and the last slot computes the wrapped fall-through itself)."""
    slots = []
    armed_branch = armed_e = stop = False
    pc = entry
    while len(slots) < MAX_SLOTS:
        if pc + 8 > region_end:
            break
        lower, upper = words(image, pc)
        maybe_branch = armed_branch
        has_e = bool(upper & 0x40000000)
        is_branch = lower_is_branch(lower, upper)
        slots.append({'pc': pc, 'lower': lower, 'upper': upper, 'next': pc + 8,
                      'flags': (MAYBE_BRANCH if maybe_branch else 0) | (ARM_E if has_e else 0)})
        if armed_e:
            stop = True
            break
        armed_e = has_e
        armed_branch = is_branch
        if maybe_branch:
            break
        pc += 8
    if slots:
        slots[-1]['flags'] |= LAST
    return slots, stop


def successors(slots, stop: bool, region_end: int) -> list[int]:
    out = []
    if stop:
        # The next program commonly starts right after this one ends.
        out.append(slots[-1]['next'])
        return out
    last = slots[-1]
    if last['flags'] & MAYBE_BRANCH and len(slots) >= 2:
        br = slots[-2]
        op = (br['lower'] >> 25) & 0x7F
        if op in STATIC_TARGET_OPS:
            out.append(br['pc'] + 8 + imm11(br['lower']) * 8)
        if op in COND_OPS or op in CALL_OPS:
            out.append(last['next'])
        return out
    # A length cap, a wrap, or a branch pending at the cap: continue at next.
    out.append(last['next'])
    return out


def seeds(image: bytes, region_end: int) -> set[int]:
    s = {0}
    for pc in range(0, region_end - 7, 8):
        lower, upper = words(image, pc)
        if upper & 0x40000000:
            s.add(pc + 16)                      # after the E bit's delay slot
        if lower_is_branch(lower, upper):
            op = (lower >> 25) & 0x7F
            if op in STATIC_TARGET_OPS:
                s.add(pc + 8 + imm11(lower) * 8)
            if op in CALL_OPS or op in COND_OPS:
                s.add(pc + 16)
    return s


def discover(image: bytes, region_end: int, entries: set[int]):
    todo = sorted(entries | seeds(image, region_end))
    done = set()
    blocks = []
    while todo:
        pc = todo.pop()
        if pc in done or pc % 8 or pc < 0 or pc + 8 > region_end:
            continue
        done.add(pc)
        slots, stop = plan(image, region_end, pc)
        if not slots:
            continue
        blocks.append((pc, slots, stop))
        for nxt in successors(slots, stop, region_end):
            if nxt not in done:
                todo.append(nxt)
    return blocks


def elf_chains(elf: bytes) -> list[bytes]:
    """Every VIF MPG upload chain in the ELF, as its concatenated code.

    A packet is `VIF NOP; MPG num,addr` (word-aligned so the code is 8-byte
    aligned) followed by num*8 bytes; a chain is packets laid back to back. A
    chain must look like VU microcode (upper-pipe NOPs / lower-pipe NOPs and
    an E bit), which excludes stray matches in EE code."""
    packets = []
    for o in range(4, len(elf) - 8, 8):
        w = struct.unpack_from('<I', elf, o)[0]
        if (w >> 24) & 0x7F != 0x4A or struct.unpack_from('<I', elf, o - 4)[0] != 0:
            continue
        num = (w >> 16) & 0xFF or 256
        addr = w & 0xFFFF
        if addr + num <= 2048 and o + 4 + num * 8 <= len(elf):
            packets.append((o, num))
    chains, cur = [], []
    for o, num in packets:
        if cur and cur[-1][0] + 4 + cur[-1][1] * 8 + 4 == o:
            cur.append((o, num))
        else:
            if cur:
                chains.append(cur)
            cur = [(o, num)]
    if cur:
        chains.append(cur)
    out = []
    for chain in chains:
        code = b''.join(elf[o + 4:o + 4 + num * 8] for o, num in chain)
        n = len(code) // 8
        nops = e_bits = 0
        for k in range(n):
            lower, upper = struct.unpack_from('<II', code, k * 8)
            nops += upper == 0x000002FF or lower == 0x8000033C
            e_bits += bool(upper & 0x40000000)
        if e_bits and nops * 20 >= n:
            out.append(code)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('record_dirs', nargs='*', type=Path)
    ap.add_argument('--elf', type=Path, help='your own dumped game ELF (static MPG extraction)')
    ap.add_argument('--output', type=Path, required=True, help='directory for rrv_vu_aot_blocks.inc')
    ap.add_argument('--no-analysis', action='store_true',
                    help='emit every per-slot step (no flag liveness, no operand-clamp elision)')
    args = ap.parse_args()

    catalogue = {}   # raw words -> (slots, stop)
    regions = {'recorded': 0, 'elf': 0}

    def add(image: bytes, region_end: int, entries: set[int]):
        for pc, slots, stop in discover(image, region_end, entries):
            raw = tuple((sl['upper'] << 32) | sl['lower'] for sl in slots)
            catalogue.setdefault(raw, (slots, stop))

    if args.elf:
        for code in elf_chains(args.elf.read_bytes()):
            regions['elf'] += 1
            # Every slot is a possible entry (MSCAL / MSCNT addresses are
            # computed by EE code), so every slot of a shipped program starts a
            # block: complete static coverage, whatever the game calls.
            add(code, len(code), set(range(0, len(code), 8)))
    for d in args.record_dirs:
        for binf in sorted(d.glob('vu*-*-*.bin')):
            _, size_s, _ = binf.stem.split('-')
            code_size = int(size_s)
            image = binf.read_bytes()
            if len(image) != code_size:
                raise SystemExit(f'{binf}: {len(image)} bytes, expected {code_size}')
            pcs_file = binf.with_suffix('.pcs')
            entries = {int(x, 16) for x in pcs_file.read_text().split()} if pcs_file.exists() else set()
            regions['recorded'] += 1
            add(image, code_size, entries)

    keys = sorted(catalogue, key=lambda k: (k[0], -len(k), k))
    digest = hashlib.sha256()
    for k in keys:
        digest.update(struct.pack('<I', len(k)))
        digest.update(struct.pack(f'<{len(k)}Q', *k))
        digest.update(bytes([catalogue[k][1]]))
    total_slots = sum(len(k) for k in keys)

    analysis = {'slots': 0, 'fmac': 0, 'no_mac_word': 0, 'no_ring_write': 0, 'no_visible': 0,
                'clamps': 0, 'clamps_skipped': 0}
    if not args.no_analysis:
        for k in keys:
            for name, value in analyse(catalogue[k][0]).items():
                analysis[name] += value
            # The id names the code the blocks compile to, so it covers what the analysis decided
            # (without the analysis it is the id of the words alone, as before).
            digest.update(struct.pack(f'<{len(k)}H', *(sl['flags'] for sl in catalogue[k][0])))
    cat_id = digest.hexdigest()[:16]

    out = []
    out.append('// GENERATED by tools/vu-aot/vu_aot_gen.py — do not edit.\n')
    out.append('// Statically recompiled VU microcode for RRV_VU_AOT (Ridge Racer V).\n')
    out.append("// Derivative of the game's microcode: private repository only.\n")
    out.append(f"// catalogue {cat_id}: {regions['elf']} ELF upload chains, "
               f"{regions['recorded']} recorded images, {len(keys)} blocks, {total_slots} slots.\n\n")
    out.append('using VuAot = VU1Interpreter::Aot;\n\n')
    entries = []
    for i, k in enumerate(keys):
        slots, stop = catalogue[k]
        name = f'vuaot_{i:05d}'
        out.append(f'static void {name}(VU1Interpreter &vu, uint32_t pc)\n{{\n    const int p = VuAot::begin(vu);\n')
        for n, sl in enumerate(slots):
            out.append(f'    VuAot::slot<0x{sl["lower"]:08x}u, 0x{sl["upper"]:08x}u, {n}u, {sl["flags"]}u>(vu, p, pc);\n')
        out.append(f'    VuAot::end<{len(slots)}u>(vu, p);\n}}\n')
        out.append(f'static const uint64_t {name}_raw[] = {{')
        out.append(', '.join(f'0x{w:016x}ull' for w in k))
        out.append('};\n\n')
        entries.append(f'    {{0x{k[0]:016x}ull, {len(k)}u, {1 if stop else 0}u, {name}_raw, &{name}}},\n')
    out.append('const VU1Interpreter::AotBlock kVuAotBlocks[] = {\n')
    out.extend(entries)
    out.append('};\n')
    out.append('constexpr size_t kVuAotBlockCount = sizeof(kVuAotBlocks) / sizeof(kVuAotBlocks[0]);\n')
    out.append(f'constexpr const char kVuAotCatalogId[] = "{cat_id}";\n')

    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'rrv_vu_aot_blocks.inc').write_text(''.join(out))
    manifest = {
        'schema': 'rrv-vu-aot-catalogue-v2',
        'catalogue_id': cat_id,
        'generator': 'tools/vu-aot/vu_aot_gen.py',
        'elf_chains': regions['elf'],
        'recorded_images': regions['recorded'],
        'blocks': len(keys),
        'slots': total_slots,
        'max_block_slots': MAX_SLOTS,
        'position_independent': True,
        'block_analysis': None if args.no_analysis else analysis,
        'input_elf_sha256': hashlib.sha256(args.elf.read_bytes()).hexdigest() if args.elf else None,
        'record_dirs': [str(d) for d in args.record_dirs],
        'blocks_inc_sha256': hashlib.sha256(''.join(out).encode()).hexdigest(),
    }
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
