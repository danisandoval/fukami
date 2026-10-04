#!/usr/bin/env python3
# retired from the source-owned product 2026-10-01 (third_party/ps2recomp + src/product hold its effect);
# still used by the legacy/diagnostic configurations (RRV_PRODUCT_OWNED_SOURCE=OFF) through cmake/Rrv*.cmake.
"""Fix PAD-001 libpad pressure negotiation without modifying the locked producer."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from m2_causal_overlay import replace_once
from m2_dma_overlay import require_locked_clean_producer
from m2p_pad_overlay import PINNED, pad_stub


PAD_SOURCE = "src/lib/Kernel/Stubs/Pad.cpp"


def patch_pad(text: str) -> str:
    # libpad Enter/ExitPressMode wrap SetButtonInfo(0xfff/0). The producer's
    # private buttonMask remains a digital-button mask; translate the response
    # mask at the API boundary instead of changing any host or runtime ABI.
    text = replace_once(text,
        "        portState->pressureEnabled = true;\n",
        "        portState->buttonMask = 0xFFFFu;\n"
        "        portState->pressureEnabled = true;\n",
        "EnterPressMode enables every pressure response")
    text = replace_once(text,
        "        portState->pressureEnabled = false;\n        portState->reqState = 0u;\n"
        "        if (runtime && runtime->padBackend().diagnosticsEnabled())\n",
        "        portState->buttonMask = 0u;\n"
        "        portState->pressureEnabled = false;\n        portState->reqState = 0u;\n"
        "        if (runtime && runtime->padBackend().diagnosticsEnabled())\n",
        "ExitPressMode disables every pressure response")
    text = replace_once(text,
        "            portState->buttonMask = static_cast<uint16_t>(getRegU32(ctx, 6));\n",
        "            // SetButtonInfo selects the twelve pressure response bytes,\n"
        "            // not the active-low digital button positions. RR5's\n"
        "            // generated EnterPressMode calls this API with 0xfff.\n"
        "            constexpr uint16_t pressureButtons[] = {\n"
        "                kPadBtnRight, kPadBtnLeft, kPadBtnUp, kPadBtnDown,\n"
        "                kPadBtnTriangle, kPadBtnCircle, kPadBtnCross, kPadBtnSquare,\n"
        "                kPadBtnL1, kPadBtnR1, kPadBtnL2, kPadBtnR2};\n"
        "            const uint32_t responseMask = getRegU32(ctx, 6) & 0x0FFFu;\n"
        "            portState->buttonMask = 0u;\n"
        "            for (unsigned index = 0; index < 12; ++index)\n"
        "            {\n"
        "                if ((responseMask & (1u << index)) != 0u)\n"
        "                    portState->buttonMask |= pressureButtons[index];\n"
        "            }\n"
        "            portState->pressureEnabled = responseMask != 0u;\n",
        "SetButtonInfo pressure mode and response mask")
    text = replace_once(text,
        "            data[17] = pressureValue(state, portState, kPadBtnL2);\n"
        "            data[18] = pressureValue(state, portState, kPadBtnR1);\n",
        "            data[17] = pressureValue(state, portState, kPadBtnR1);\n"
        "            data[18] = pressureValue(state, portState, kPadBtnL2);\n",
        "DualShock 2 pressure response shoulder order")
    return text


def generate(runtime_source: Path, source: Path, output_dir: Path) -> None:
    require_locked_clean_producer(runtime_source)
    original = (runtime_source / PAD_SOURCE).read_bytes()
    original_hash = hashlib.sha256(original).hexdigest()
    if original_hash != PINNED[PAD_SOURCE]:
        raise RuntimeError("PAD-001 requires the SHA-pinned Pad.cpp producer")
    raw = source.read_bytes()
    # Accept exactly the original or the known observer transformation. A
    # different or stale overlay must fail instead of silently losing changes.
    if raw == original:
        input_kind = "locked-producer"
    elif raw == pad_stub(original.decode("utf-8"))[0].encode("utf-8"):
        input_kind = "m2p-pad-observer"
    else:
        raise RuntimeError("PAD-001 input is neither the pinned producer nor its pad observer overlay")
    patched = patch_pad(raw.decode("utf-8")).encode("utf-8")
    destination = output_dir / PAD_SOURCE
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(patched)
    manifest = {
        "schema_version": 1,
        "overlay": "PAD-001 pressure negotiation",
        "input_kind": input_kind,
        "files": [{
            "path": PAD_SOURCE,
            "producer_sha256": original_hash,
            "input_sha256": hashlib.sha256(raw).hexdigest(),
            "overlay_sha256": hashlib.sha256(patched).hexdigest(),
        }],
    }
    (output_dir / "pad-pressure-overlay-manifest.json").write_text(
        json.dumps(manifest, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-source", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    generate(args.runtime_source, args.source, args.output_dir)


if __name__ == "__main__":
    main()
