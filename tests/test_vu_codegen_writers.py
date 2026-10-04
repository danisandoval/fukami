#!/usr/bin/env python3
"""Gate 5: every writer of VU0/VU1 micro memory bumps g_rrvVuCodeGen.

The VU AOT block cache (src/vu-aot/rrv_vu_aot_engine.inc, aotLookup) skips its per-dispatch memcmp for
a block validated at the current micro-memory generation. That is only sound if no code path changes
m_vu0Code / m_vu1Code without calling rrvVuCodeChanged(unit). This test reads the runtime sources and
fails when a store into either array has no bump next to it, so a new writer cannot slip in unnoticed.
RRV_VU_AOT_GENCHECK=1 is the run-time cross-check of the same rule.
"""
from __future__ import annotations

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / "third_party/ps2recomp/ps2xRuntime"
MEMORY = RT / "src/lib/ps2_memory.cpp"
VIF = {0: RT / "src/lib/ps2_vif0_interpreter.cpp", 1: RT / "src/lib/ps2_vif1_interpreter.cpp"}
HEADER = RT / "include/runtime/ps2_memory.h"

# A line that stores into a micro-memory array (loads are not matched).
STORE = re.compile(
    r"storeScalar<\w+>\(\s*m_vu(?P<a>[01])Code\b"
    r"|m_vu(?P<b>[01])Code\[[^\]]+\]\s*=(?!=)"
    r"|_mm_storeu_si128\([^;]*m_vu(?P<c>[01])Code\b"
    r"|std::memset\(\s*m_vu(?P<d>[01])Code\b"
    r"|std::memcpy\(\s*m_vu(?P<e>[01])Code\b")
WINDOW = 6  # lines after the store in which the bump must appear


def stores(path: Path):
    lines = path.read_text().splitlines()
    for number, line in enumerate(lines):
        match = STORE.search(line)
        if match:
            unit = int(next(g for g in match.groups() if g is not None))
            yield number, unit, lines


class VuCodeGenerationWriters(unittest.TestCase):
    def test_counter_is_declared(self):
        text = HEADER.read_text()
        self.assertIn("g_rrvVuCodeGen[2]", text)
        self.assertIn("rrvVuCodeChanged(int unit)", text)

    def test_memory_writers_bump_their_unit(self):
        found = 0
        for number, unit, lines in stores(MEMORY):
            found += 1
            near = "\n".join(lines[number:number + WINDOW])
            self.assertIn(f"rrvVuCodeChanged({unit});", near,
                          f"{MEMORY.name}:{number + 1}: store into VU{unit} micro memory without a generation bump")
        self.assertGreaterEqual(found, 8)  # init memset x2, write8/16/32/128 for VU0, write32/128 for VU1

    def test_mpg_uploads_bump_only_when_bytes_change(self):
        for unit, path in VIF.items():
            hits = list(stores(path))
            self.assertEqual(len(hits), 1, f"{path.name}: expected exactly one micro-memory store")
            number, _, lines = hits[0]
            self.assertIn("std::memcmp(", lines[number - 2] + lines[number - 1],
                          f"{path.name}: the upload must compare before it copies")
            self.assertIn(f"rrvVuCodeChanged({unit});", "\n".join(lines[number:number + WINDOW]))

    def test_no_other_translation_unit_writes_micro_memory(self):
        # getVU0Code()/getVU1Code() hand out raw pointers; only reads may use them.
        for path in list((RT / "src").rglob("*.cpp")) + list((ROOT / "src").rglob("*.cpp")):
            if path in (MEMORY, *VIF.values()) or "ps2xTest" in str(path):
                continue
            for number, line in enumerate(path.read_text().splitlines()):
                if re.search(r"(memcpy|memset|memmove)\(\s*(m_memory\.)?getVU[01]Code\(\)", line):
                    self.fail(f"{path}:{number + 1}: writes VU micro memory through getVU*Code()")


if __name__ == "__main__":
    unittest.main()
