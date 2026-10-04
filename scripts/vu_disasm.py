#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+
# Provenance: opcode tables transcribed from PCSX2 d5f75c9e4 (GPL-3.0+), pcsx2/VUops.cpp
# _vuTablesMess and the per-op handlers it names; licence GPL-3.0+ (PCSX2 Dev Team).
"""vu_disasm.py — disassemble a VU1 microprogram image.

Reads a raw 16 KB VU1 micro-memory dump (as produced by RRV_VU_INTRACE's
`*_micro_*.bin`, or PCSX2's equivalent) and prints VU upper/lower instruction
pairs.  Opcode tables are transcribed from the pinned PCSX2 checkout
(`pcsx2/VUops.cpp` @ d5f75c9e4, `_vuTablesMess`), which is the project's
reference implementation for PS2 behaviour.

Usage:
    scripts/vu_disasm.py <micro.bin> [--start 0xc0] [--end 0x2600]
    scripts/vu_disasm.py <micro.bin> --entry 0xc0        # follow to the last E-bit
    scripts/vu_disasm.py <micro.bin> --entry 0xc0 --uses vf3
"""
import argparse
import struct
import sys

BC = "xyzw"

UPPER = {}
for i, n in enumerate(("ADD", "SUB", "MADD", "MSUB", "MAX", "MINI", "MUL")):
    for b in range(4):
        UPPER[i * 4 + b] = (n + BC[b], "bc")
UPPER.update({
    0x1C: ("MULq", "q"), 0x1D: ("MAXi", "i"), 0x1E: ("MULi", "i"), 0x1F: ("MINIi", "i"),
    0x20: ("ADDq", "q"), 0x21: ("MADDq", "q"), 0x22: ("ADDi", "i"), 0x23: ("MADDi", "i"),
    0x24: ("SUBq", "q"), 0x25: ("MSUBq", "q"), 0x26: ("SUBi", "i"), 0x27: ("MSUBi", "i"),
    0x28: ("ADD", "fd"), 0x29: ("MADD", "fd"), 0x2A: ("MUL", "fd"), 0x2B: ("MAX", "fd"),
    0x2C: ("SUB", "fd"), 0x2D: ("MSUB", "fd"), 0x2E: ("OPMSUB", "fd"), 0x2F: ("MINI", "fd"),
})

# (code >> 6) & 0x1f, one table per 0x3C..0x3F
FD = [
    ["ADDAx", "SUBAx", "MADDAx", "MSUBAx", "ITOF0", "FTOI0", "MULAx", "MULAq",
     "ADDAq", "SUBAq", "ADDA", "SUBA"],
    ["ADDAy", "SUBAy", "MADDAy", "MSUBAy", "ITOF4", "FTOI4", "MULAy", "ABS",
     "MADDAq", "MSUBAq", "MADDA", "MSUBA"],
    ["ADDAz", "SUBAz", "MADDAz", "MSUBAz", "ITOF12", "FTOI12", "MULAz", "MULAi",
     "ADDAi", "SUBAi", "MULA", "OPMULA"],
    ["ADDAw", "SUBAw", "MADDAw", "MSUBAw", "ITOF15", "FTOI15", "MULAw", "CLIP",
     "MADDAi", "MSUBAi", None, "NOP"],
]

LOWER = {
    0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU",
    0x10: "FCEQ", 0x11: "FCSET", 0x12: "FCAND", 0x13: "FCOR",
    0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR",
    0x18: "FMEQ", 0x1A: "FMAND", 0x1B: "FMOR", 0x1C: "FCGET",
    0x20: "B", 0x21: "BAL", 0x24: "JR", 0x25: "JALR",
    0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ",
}
LOWER_OP = {0x30: "IADD", 0x31: "ISUB", 0x32: "IADDI", 0x34: "IAND", 0x35: "IOR"}
T3 = [
    {0xC: "MOVE", 0xD: "LQI", 0xE: "DIV", 0xF: "MTIR", 0x10: "RNEXT",
     0x19: "MFP", 0x1A: "XTOP", 0x1B: "XGKICK",
     0x1C: "ESADD", 0x1D: "EATANxy", 0x1E: "ESQRT", 0x1F: "ESIN"},
    {0xC: "MR32", 0xD: "SQI", 0xE: "SQRT", 0xF: "MFIR", 0x10: "RGET",
     0x1A: "XITOP",
     0x1C: "ERSADD", 0x1D: "EATANxz", 0x1E: "ERSQRT", 0x1F: "EATAN"},
    {0xD: "LQD", 0xE: "RSQRT", 0xF: "ILWR", 0x10: "RINIT",
     0x1C: "ELENG", 0x1D: "ESUM", 0x1E: "ERCPR", 0x1F: "EEXP"},
    {0xD: "SQD", 0xE: "WAITQ", 0xF: "ISWR", 0x10: "RXOR",
     0x1C: "ERLENG", 0x1E: "WAITP"},
]


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def dest(code):
    d = (code >> 21) & 0xF
    return "".join(c for c, b in zip("xyzw", (8, 4, 2, 1)) if d & b) or "0"


def dis_upper(code):
    op = code & 0x3F
    ft, fs, fd = (code >> 16) & 0x1F, (code >> 11) & 0x1F, (code >> 6) & 0x1F
    d = dest(code)
    if op >= 0x3C:
        idx = (code >> 6) & 0x1F
        tbl = FD[op - 0x3C]
        name = tbl[idx] if idx < len(tbl) else None
        if name is None:
            return f"?upper_{op:02x}_{idx:02x}", [], []
        if name in ("ITOF0", "ITOF4", "ITOF12", "ITOF15", "FTOI0", "FTOI4",
                    "FTOI12", "FTOI15", "ABS", "MOVE"):
            return f"{name}.{d} vf{ft:02d}, vf{fs:02d}", [f"vf{ft}"], [f"vf{fs}"]
        if name == "CLIP":
            return f"CLIP vf{fs:02d}xyz, vf{ft:02d}w", ["clip"], [f"vf{fs}", f"vf{ft}"]
        if name.endswith(("x", "y", "z", "w")) and name[:-1].endswith("A"):
            return f"{name}.{d} ACC, vf{fs:02d}, vf{ft:02d}{name[-1]}", ["ACC"], [f"vf{fs}", f"vf{ft}"]
        if name.endswith("i") or name.endswith("q"):
            return f"{name}.{d} ACC, vf{fs:02d}, {name[-1].upper()}", ["ACC"], [f"vf{fs}"]
        if name == "NOP":
            return "NOP", [], []
        return f"{name}.{d} ACC, vf{fs:02d}, vf{ft:02d}", ["ACC"], [f"vf{fs}", f"vf{ft}"]
    name, kind = UPPER[op]
    if kind == "bc":
        return (f"{name}.{d} vf{fd:02d}, vf{fs:02d}, vf{ft:02d}{name[-1]}",
                [f"vf{fd}"], [f"vf{fs}", f"vf{ft}"])
    if kind in ("q", "i"):
        return (f"{name}.{d} vf{fd:02d}, vf{fs:02d}, {kind.upper()}",
                [f"vf{fd}"], [f"vf{fs}"])
    return (f"{name}.{d} vf{fd:02d}, vf{fs:02d}, vf{ft:02d}",
            [f"vf{fd}"], [f"vf{fs}", f"vf{ft}"])


def dis_lower(code, pc):
    op = code >> 25
    ft, fs, fd = (code >> 16) & 0x1F, (code >> 11) & 0x1F, (code >> 6) & 0x1F
    it, is_, id_ = ft, fs, fd
    d = dest(code)
    imm11 = sx(code & 0x7FF, 11)
    fsf, ftf = (code >> 21) & 3, (code >> 23) & 3
    if op == 0x40:
        o2 = code & 0x3F
        if o2 in LOWER_OP:
            n = LOWER_OP[o2]
            if n == "IADDI":
                return f"IADDI vi{it:02d}, vi{is_:02d}, {sx((code >> 6) & 0x1F, 5)}", [f"vi{it}"], [f"vi{is_}"]
            return f"{n} vi{id_:02d}, vi{is_:02d}, vi{it:02d}", [f"vi{id_}"], [f"vi{is_}", f"vi{it}"]
        if o2 >= 0x3C:
            idx = (code >> 6) & 0x1F
            n = T3[o2 - 0x3C].get(idx)
            if n is None:
                return f"?lowerop_{o2:02x}_{idx:02x}", [], []
            if n in ("MOVE", "MR32"):
                return f"{n}.{d} vf{ft:02d}, vf{fs:02d}", [f"vf{ft}"], [f"vf{fs}"]
            if n in ("LQI", "LQD"):
                return f"{n}.{d} vf{ft:02d}, (vi{is_:02d}{'++' if n == 'LQI' else '--'})", [f"vf{ft}", f"vi{is_}"], [f"vi{is_}"]
            if n in ("SQI", "SQD"):
                return f"{n}.{d} vf{fs:02d}, (vi{it:02d}{'++' if n == 'SQI' else '--'})", [f"vi{it}"], [f"vf{fs}", f"vi{it}"]
            if n == "DIV":
                return f"DIV Q, vf{fs:02d}{BC[fsf]}, vf{ft:02d}{BC[ftf]}", ["Q"], [f"vf{fs}", f"vf{ft}"]
            if n in ("SQRT",):
                return f"SQRT Q, vf{ft:02d}{BC[ftf]}", ["Q"], [f"vf{ft}"]
            if n == "RSQRT":
                return f"RSQRT Q, vf{fs:02d}{BC[fsf]}, vf{ft:02d}{BC[ftf]}", ["Q"], [f"vf{fs}", f"vf{ft}"]
            if n == "MTIR":
                return f"MTIR vi{it:02d}, vf{fs:02d}{BC[fsf]}", [f"vi{it}"], [f"vf{fs}"]
            if n == "MFIR":
                return f"MFIR.{d} vf{ft:02d}, vi{is_:02d}", [f"vf{ft}"], [f"vi{is_}"]
            if n in ("ILWR",):
                return f"ILWR.{d} vi{it:02d}, (vi{is_:02d})", [f"vi{it}"], [f"vi{is_}"]
            if n in ("ISWR",):
                return f"ISWR.{d} vi{it:02d}, (vi{is_:02d})", [], [f"vi{it}", f"vi{is_}"]
            if n in ("XTOP", "XITOP"):
                return f"{n} vi{it:02d}", [f"vi{it}"], []
            if n == "XGKICK":
                return f"XGKICK vi{is_:02d}", [], [f"vi{is_}"]
            if n in ("RINIT", "RXOR"):
                return f"{n} R, vf{fs:02d}{BC[fsf]}", ["R"], [f"vf{fs}"]
            if n in ("RGET", "RNEXT"):
                return f"{n}.{d} vf{ft:02d}, R", [f"vf{ft}"], ["R"]
            if n == "MFP":
                return f"MFP.{d} vf{ft:02d}, P", [f"vf{ft}"], ["P"]
            if n in ("WAITQ", "WAITP"):
                return n, [], []
            # EFU ops
            if n in ("ESADD", "ERSADD", "ELENG", "ERLENG", "ESUM"):
                return f"{n} P, vf{fs:02d}", ["P"], [f"vf{fs}"]
            return f"{n} P, vf{fs:02d}{BC[fsf]}", ["P"], [f"vf{fs}"]
        return f"?lowerop_{o2:02x}", [], []
    n = LOWER.get(op)
    if n is None:
        return f"?lower_{op:02x}", [], []
    if n == "LQ":
        return f"LQ.{d} vf{ft:02d}, {imm11}(vi{is_:02d})", [f"vf{ft}"], [f"vi{is_}"]
    if n == "SQ":
        return f"SQ.{d} vf{fs:02d}, {imm11}(vi{it:02d})", [], [f"vf{fs}", f"vi{it}"]
    if n == "ILW":
        return f"ILW.{d} vi{it:02d}, {imm11}(vi{is_:02d})", [f"vi{it}"], [f"vi{is_}"]
    if n == "ISW":
        return f"ISW.{d} vi{it:02d}, {imm11}(vi{is_:02d})", [], [f"vi{it}", f"vi{is_}"]
    if n in ("IADDIU", "ISUBIU"):
        imm15 = ((code >> 10) & 0x7800) | (code & 0x7FF)
        return f"{n} vi{it:02d}, vi{is_:02d}, {imm15}", [f"vi{it}"], [f"vi{is_}"]
    if n in ("B", "BAL"):
        tgt = pc + 8 + imm11 * 8
        return f"{n} 0x{tgt:04x}" + (f" (vi{it:02d})" if n == "BAL" else ""), [], []
    if n in ("JR", "JALR"):
        return f"{n} vi{is_:02d}", [], [f"vi{is_}"]
    if n in ("IBEQ", "IBNE"):
        return f"{n} vi{is_:02d}, vi{it:02d}, 0x{pc + 8 + imm11 * 8:04x}", [], [f"vi{is_}", f"vi{it}"]
    if n.startswith("IB"):
        return f"{n} vi{is_:02d}, 0x{pc + 8 + imm11 * 8:04x}", [], [f"vi{is_}"]
    if n.startswith("FC") or n.startswith("FS") or n.startswith("FM"):
        return f"{n} vi{it:02d}, 0x{code & 0xFFF:03x}", [f"vi{it}"], []
    return n, [], []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--start", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--end", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--entry", type=lambda s: int(s, 0), default=None,
                    help="disassemble from entry until the instruction after the E bit")
    ap.add_argument("--uses", default=None, help="only print lines touching this register")
    args = ap.parse_args()

    img = open(args.image, "rb").read()
    start = args.entry if args.entry is not None else args.start
    end = args.end if args.end is not None else len(img)
    pc = start
    stop_at = None
    while pc < end and pc + 8 <= len(img):
        lower, upper = struct.unpack_from("<II", img, pc)
        ubits = ""
        for bit, ch in ((31, "I"), (30, "E"), (29, "M"), (28, "D"), (27, "T")):
            if upper & (1 << bit):
                ubits += ch
        if upper & (1 << 31):
            ltxt = f"LOI {struct.unpack('<f', struct.pack('<I', lower))[0]:g}"
            lw, lr = [], []
        else:
            ltxt, lw, lr = dis_lower(lower, pc)
        utxt, uw, ur = dis_upper(upper)
        line = f"{pc:04x}: {upper:08x} {lower:08x}  {ubits:<3} {utxt:<34} {ltxt}"
        if args.uses is None or args.uses in (uw + ur + lw + lr):
            print(line)
        if args.entry is not None and (upper & (1 << 30)) and stop_at is None:
            stop_at = pc + 16  # E bit: two more instructions issue
        if stop_at is not None and pc >= stop_at:
            break
        pc += 8


if __name__ == "__main__":
    sys.exit(main())
