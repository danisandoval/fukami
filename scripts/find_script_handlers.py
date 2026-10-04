#!/usr/bin/env python3
"""find_script_handlers.py — enumerate state-script handlers the recompiler merged.

RRV's state machine dispatches per-step handlers through 2-level pointer tables in
the data section (clustered around 0x3147xx-0x3149xx). Many of those handler
addresses are reached ONLY via the indirect `jalr`, which static flow analysis
can't follow, so the recompiler folds them into a neighbouring function with no
re-entry label. At runtime the dispatcher then hits an unmapped PC
([dispatch:first-bad-pc]) and the calling thread dies.

This script scans the table region of the *user-supplied* boot ELF for code
pointers and reports the ones that need carving out as their own functions — i.e.
a pointer whose containing generated function has NO re-entry `case`/`label` for
it. Feed the result into config/rrv.toml [general] force_functions, then regen
(the recompiler force_functions patch must be applied — see
tools/patches/ps2recomp-force-functions.patch).

Usage:
    python3 scripts/find_script_handlers.py [elf] [output_dir] [lo hi]
Defaults: local/rrv_boot.elf, config/output/, scan 0x314000-0x315c00.

Nothing here is game data — it only reads addresses from the user's own ELF and
the locally generated C++. Both are gitignored; this script is the tracked recipe.
"""
import sys, os, re, glob, struct, bisect

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
elf_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "local/rrv_boot.elf")
out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, "config/output")
scan_lo = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x314000
scan_hi = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x315c00
CODE_LO, CODE_HI = 0x200000, 0x2e0000  # EE .text range to accept as a handler target

elf = open(elf_path, "rb").read()
e_phoff = struct.unpack_from("<I", elf, 0x1c)[0]
e_phentsize = struct.unpack_from("<H", elf, 0x2a)[0]
e_phnum = struct.unpack_from("<H", elf, 0x2c)[0]
segs = []
for i in range(e_phnum):
    o = e_phoff + i * e_phentsize
    p_type, p_off, p_vaddr, _, p_filesz, _ = struct.unpack_from("<IIIIII", elf, o)
    if p_type == 1:  # PT_LOAD
        segs.append((p_vaddr, p_off, p_filesz))


def w32(v):
    for vad, off, fsz in segs:
        if vad <= v < vad + fsz:
            return struct.unpack_from("<I", elf, off + (v - vad))[0]
    return None


# Map every generated function: start -> (end, path)
funcs = []
for f in glob.glob(os.path.join(out_dir, "sub_*_0x*.cpp")):
    m = re.search(r"_0x([0-9a-fA-F]+)\.cpp$", f)
    if not m:
        continue
    start = int(m.group(1), 16)
    end = None
    with open(f) as fh:
        for _ in range(20):
            line = fh.readline()
            mm = re.search(r"Address:\s*0x([0-9a-fA-F]+)\s*-\s*0x([0-9a-fA-F]+)", line)
            if mm:
                end = int(mm.group(2), 16)
                break
    funcs.append((start, end, f))
funcs.sort()
starts = [s for s, _, _ in funcs]


def containing(addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0:
        return None
    s, e, f = funcs[i]
    if e is not None and addr >= e:
        return None
    return funcs[i]


# Collect code-pointer targets in the table region, then keep only those that
# are neither a function start nor an internal branch target of their container.
need, works = [], []
seen = set()
for v in range(scan_lo, scan_hi, 4):
    p = w32(v)
    if p is None or not (CODE_LO <= p < CODE_HI) or (p & 3) or p in seen:
        continue
    seen.add(p)
    if p in starts:
        continue
    c = containing(p)
    if c is None:
        continue
    s, e, f = c
    txt = open(f).read()
    if f"case 0x{p:x}u:" in txt or f"label_{p:x}:" in txt:
        works.append((p, s))      # dispatcher already routes here — leave alone
    else:
        need.append((p, s))       # merged with no re-entry — needs force-split

print(f"scanned 0x{scan_lo:x}-0x{scan_hi:x}; {len(works)} already-routed, {len(need)} need splitting\n")
if need:
    print("force_functions additions (config/rrv.toml [general]):")
    print("  " + ", ".join(f'"0x{p:08X}"' for p, _ in sorted(need)))
    print("\ndetail (addr -> merged into):")
    for p, s in sorted(need):
        print(f"  0x{p:08x}  inside sub_{s:08X}")
