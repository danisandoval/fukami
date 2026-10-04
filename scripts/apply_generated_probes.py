#!/usr/bin/env python3
"""Inject tracked, default-off diagnostics into gitignored recompiled sources."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "config" / "output"

def inject(name: str, anchors):
    path = OUT / name
    text = path.read_text()
    include = '#include "rrv_guest_state_trace.h"\n'
    if include not in text:
        needle = '#include "ps2_runtime.h"\n'
        if text.count(needle) != 1:
            raise SystemExit(f"{path}: expected one runtime include")
        text = text.replace(needle, needle + include, 1)
    for needle, addition in anchors:
        if addition in text:
            continue
        if text.count(needle) != 1:
            raise SystemExit(f"{path}: probe anchor changed: {needle.strip()}")
        text = text.replace(needle, addition + needle, 1)
    path.write_text(text)
    print(f"generated probe: {path.name}")

inject("sub_0021DE90_0x21de90.cpp", [
    ("    // 0x21df94: 0x80f809  jalr", 
     "    rrv::guesttrace::dispatch(rdram, ctx, 0x21DF94u);\n")])
inject("sub_0021E320_0x21e320.cpp", [
    ("    // 0x21e464: 0xaf80a774", "    rrv::guesttrace::flagWrite(rdram, ctx, 0x21E464u, 0u);\n")])
inject("sub_0021E658_0x21e658.cpp", [
    ("    // 0x21e678: 0xaf83a774", "    rrv::guesttrace::flagWrite(rdram, ctx, 0x21E678u, GPR_U32(ctx, 3));\n")])
inject("sub_00220768_0x220768.cpp", [
    ("    // 0x220824: 0xaf82a774", "    rrv::guesttrace::flagWrite(rdram, ctx, 0x220824u, GPR_U32(ctx, 2));\n")])
inject("sub_002456C8_0x2456c8.cpp", [
    ("    // 0x2456c8: 0x27bdffe0", "    rrv::guesttrace::path(rdram, ctx, 0x2456C8u);\n"),
    ("    // 0x245810: 0x8087f3a", "    rrv::guesttrace::path(rdram, ctx, 0x245810u);\n")])
