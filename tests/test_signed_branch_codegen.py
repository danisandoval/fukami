#!/usr/bin/env python3
"""Regression contract for R5900 signed-zero branch code generation.

The source path is explicit so this can be run against the preserved v1
producer (which must fail) and the reconstructed v2 producer (which must
pass).  It deliberately contains only synthetic register values.
"""

from __future__ import annotations

import argparse
import pathlib
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = REPOSITORY / "build-deps" / "ps2recomp-d52-compatible-v2"
CODEGEN_RELATIVE = pathlib.Path("ps2xRecomp/src/lib/code_generator.cpp")

EXPECTED_PREDICATES = (
    'case OPCODE_BLEZ:\n                conditionStr = fmt::format("GPR_S64(ctx, {}) <= 0", rs_reg);',
    'case OPCODE_BGTZ:\n                conditionStr = fmt::format("GPR_S64(ctx, {}) > 0", rs_reg);',
    'case OPCODE_BLEZL:\n                conditionStr = fmt::format("GPR_S64(ctx, {}) <= 0", rs_reg);',
    'case OPCODE_BGTZL:\n                conditionStr = fmt::format("GPR_S64(ctx, {}) > 0", rs_reg);',
    'case REGIMM_BLTZ:\n                    conditionStr = fmt::format("GPR_S64(ctx, {}) < 0", rs_reg);',
    'case REGIMM_BGEZ:\n                    conditionStr = fmt::format("GPR_S64(ctx, {}) >= 0", rs_reg);',
    'case REGIMM_BLTZL:\n                    conditionStr = fmt::format("GPR_S64(ctx, {}) < 0", rs_reg);',
    'case REGIMM_BGEZL:\n                    conditionStr = fmt::format("GPR_S64(ctx, {}) >= 0", rs_reg);',
    'case REGIMM_BLTZAL:\n                    conditionStr = fmt::format("GPR_S64(ctx, {}) < 0", rs_reg);',
    'case REGIMM_BGEZAL:\n                    conditionStr = fmt::format("GPR_S64(ctx, {}) >= 0", rs_reg);',
    'case REGIMM_BLTZALL:\n                    conditionStr = fmt::format("GPR_S64(ctx, {}) < 0", rs_reg);',
    'case REGIMM_BGEZALL:\n                    conditionStr = fmt::format("GPR_S64(ctx, {}) >= 0", rs_reg);',
)


class SignedBranchCodegenTest(unittest.TestCase):
    source_root: pathlib.Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = (cls.source_root / CODEGEN_RELATIVE).read_text(encoding="utf-8")
        start = cls.source.index("// Conditional Branches")
        end = cls.source.index("case OPCODE_COP1:", start)
        cls.signed_branch_block = cls.source[start:end]

    def test_all_twelve_signed_zero_predicates_read_full_gprs(self) -> None:
        self.assertEqual(
            self.signed_branch_block.count("GPR_S64(ctx, {})"),
            len(EXPECTED_PREDICATES),
            "each BLEZ/BGTZ/REGIMM signed-zero form must read the full signed 64-bit GPR",
        )
        self.assertNotIn(
            "GPR_S32(ctx, {})",
            self.signed_branch_block,
            "signed-zero branches must not truncate the R5900 GPR to 32 bits",
        )

    def test_all_twelve_predicates_keep_their_original_comparison(self) -> None:
        for expected in EXPECTED_PREDICATES:
            with self.subTest(expected=expected):
                self.assertIn(expected, self.signed_branch_block)

    def test_emission_preserves_delay_slot_and_link_placement(self) -> None:
        block = self.source[self.source.index("const std::string branchTakenVar"):self.source.index("return ss.str();", self.source.index("const std::string branchTakenVar"))]
        self.assertLess(
            block.index("if (!unconditionalLinkCode.empty())"),
            block.index("if (isLikely)"),
            "non-likely AL links remain unconditional and precede branch flow",
        )
        likely = block[block.index("if (isLikely)"):block.index("else\n            {", block.index("if (isLikely)"))]
        taken_emit = 'ss << "        if (" << branchTakenVar << ") {\\n";'
        self.assertLess(likely.index(taken_emit), likely.index("delaySlotCode"))
        self.assertLess(likely.index("conditionalLinkCode"), likely.index("delaySlotCode"))
        non_likely = block[block.index("else\n            {"):]
        self.assertLess(non_likely.index("delaySlotCode"), non_likely.index(taken_emit))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, default=DEFAULT_SOURCE)
    args, remaining = parser.parse_known_args()
    SignedBranchCodegenTest.source_root = args.source.resolve()
    if not (SignedBranchCodegenTest.source_root / CODEGEN_RELATIVE).is_file():
        raise SystemExit(f"missing code generator: {SignedBranchCodegenTest.source_root / CODEGEN_RELATIVE}")
    unittest.main(argv=[__file__, *remaining])


if __name__ == "__main__":
    main()
