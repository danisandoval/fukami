#!/usr/bin/env python3
"""Verify that the M1 product graph stays SDL/Metal direct-present only."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys


FORBIDDEN = (
    "src/ir/",
    "src/gs-frame/",
    "tools/ir-metal/",
    "rrv_live_full_frame_adapter",
    "RRV_P3_",
)


def text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def source_gate(root: pathlib.Path) -> list[str]:
    problems: list[str] = []
    lock = root / "config/dependencies.lock.toml"
    patch_match = re.search(r'(?m)^compatible_patch\s*=\s*"([^"]+)"\s*$', text(lock)) if lock.is_file() else None
    producer_patch_relative = patch_match.group(1) if patch_match else None
    if not producer_patch_relative:
        problems.append("dependency lock is missing compatible_patch")
    required = {
        "src/host/rrv_sdl_presentation.cpp": (
            "SDL_WINDOW_METAL", "SDL_WINDOW_RESIZABLE", "SDL_WINDOW_ALLOW_HIGHDPI",
            "SDL_Metal_CreateView", "SDL_Metal_GetLayer", "SDL_Metal_GetDrawableSize",
            "SDL_Metal_DestroyView", "SDL_PollEvent", "SDL_GameController"),
        "src/main_product.cpp": (
            "presentation=direct GPU", "continuous CPU readback=disabled",
            "F7/P3 linked=no", "DirectGpu", "setExternalBackendForProcess",
            "input/event owner=SDL 2.32.10", "RendererKind::Metal",
            "RRV_TEST_PRESENT_LIMIT"),
        "cmake/RrvProduct.cmake": ("release-2.32.10", "rrv-product", "rrv-legacy-live"),
    }
    if producer_patch_relative:
        required[producer_patch_relative] = (
            "HostPresentationConfig", "serviceDirectPresentation",
            "setExternalBackendForProcess", "raylib window/upload/present disabled")
    for relative, needles in required.items():
        candidate = root / relative
        if not candidate.is_file():
            problems.append(f"missing required product source: {relative}")
            continue
        contents = text(candidate)
        for needle in needles:
            if needle not in contents:
                problems.append(f"{relative} is missing {needle!r}")
    lock = root / "config/dependencies.lock.toml"
    if not lock.is_file():
        problems.append("missing SDL dependency lock")
    else:
        contents = text(lock)
        for needle in ("[sdl]", 'version = "2.32.10"',
                       'tag = "release-2.32.10"',
                       'revision = "5d249570393f7a37e037abf22cd6012a4cc56a71"'):
            if needle not in contents:
                problems.append(f"SDL lock is missing immutable provenance {needle!r}")
    host = root / "src/host/rrv_sdl_presentation.cpp"
    if host.is_file():
        contents = text(host)
        for forbidden in ("NSApplication", "NSWindow", "NSView", "CAMetalLayer", "raylib", "SDL_Renderer", "SDL_GPU"):
            if forbidden in contents:
                problems.append(f"SDL product host contains forbidden platform/second-renderer token {forbidden!r}")
        if contents.count("SDL_CreateWindow(") != 1:
            problems.append("SDL product host must create exactly one SDL_Window")
        snapshot_begin = contents.find("HostPadState snapshot(unsigned port) override")
        snapshot_end = contents.find("HostPadCapabilities capabilities", snapshot_begin)
        snapshot_body = contents[snapshot_begin:snapshot_end]
        if snapshot_begin < 0 or "SDL_" in snapshot_body:
            problems.append("SDL product pad snapshot must read the cached state without SDL calls")
        capabilities_begin = snapshot_end
        capabilities_end = contents.find("private:", capabilities_begin)
        capabilities_body = contents[capabilities_begin:capabilities_end]
        if capabilities_begin < 0 or "SDL_" in capabilities_body:
            problems.append("SDL product pad capabilities must read the cached state without SDL calls")
    for relative in ("src/main_product.cpp", "cmake/RrvProduct.cmake"):
        candidate = root / relative
        if candidate.is_file():
            contents = text(candidate)
            for forbidden in ("UploadFrame", "copyFrame", "UpdateTexture", "DrawTexturePro", "EndDrawing", "InitWindow"):
                if forbidden in contents:
                    problems.append(f"{relative} contains legacy presentation token {forbidden!r}")
    producer_patch = root / producer_patch_relative if producer_patch_relative else None
    if producer_patch is not None and producer_patch.is_file():
        contents = text(producer_patch)
        direct_begin = contents.find("void PS2Runtime::serviceDirectPresentation()")
        direct_end = contents.find("void PS2Runtime::captureDirectPresentationIfRequested(")
        direct_body = contents[direct_begin:direct_end] if direct_begin >= 0 and direct_end > direct_begin else ""
        if not direct_body:
            problems.append("producer patch does not contain the direct presentation service body")
        elif any(token in direct_body for token in ("copyFrame", "UpdateTexture", "DrawTexturePro", "EndDrawing")):
            problems.append("direct presentation service contains a legacy readback/upload/present operation")
        advance_begin = contents.find("void PS2Runtime::advanceActiveGsBackendField(uint64_t fieldIndex)")
        advance_end = contents.find("#if !defined(PS2X_RRV_FIELD_ONLY)", advance_begin)
        advance_body = contents[advance_begin:advance_end]
        if advance_begin < 0 or "captureDirectPresentationIfRequested(fieldIndex)" not in advance_body:
            problems.append("explicit direct capture is not serialized with the field transition")
        if "captureDirectPresentationIfRequested" in direct_body:
            problems.append("SDL event servicing must not select/tag direct captures")
        capture_begin = contents.find("void PS2Runtime::captureDirectPresentationIfRequested(")
        capture_end = contents.find("void PS2Runtime::run()", capture_begin)
        capture_body = contents[capture_begin:capture_end]
        if capture_begin < 0 or "before any direct GPU present" not in capture_body:
            problems.append("selected direct capture ticks must fail closed without a drawable")
    main_source = root / "src/main_product.cpp"
    if main_source.is_file():
        contents = text(main_source)
        for needle in ("requestedCaptures == stats.completedCaptures",
                       "synchronousCpuReadbacks == stats.completedCaptures",
                       "cpuWaits == stats.completedCaptures",
                       "unexpectedReadbacks != 0u"):
            if needle not in contents:
                problems.append(f"product capture accounting is missing {needle!r}")
    bridge_header = root / "tools/pcsx2-gs-bridge/rrv_pcsx2_gs_bridge.h"
    bridge_impl = root / "tools/pcsx2-gs-bridge/bridge.cpp"
    if bridge_header.is_file() and "uint32_t renderer_kind" not in text(bridge_header):
        problems.append("bridge ABI config does not carry an explicit renderer request")
    if bridge_impl.is_file() and "ResolveRenderer(requested_renderer" not in text(bridge_impl):
        problems.append("bridge does not consume the explicit ABI renderer request")
    return problems


def output(*args: str) -> str:
    return subprocess.run(args, text=True, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT).stdout


def binary_gate(args: argparse.Namespace) -> list[str]:
    problems: list[str] = []
    commands = json.loads((args.build_dir / "compile_commands.json").read_text())
    bad_sources = sorted({entry["file"] for entry in commands
                          if any(token in entry["file"] for token in FORBIDDEN)})
    if bad_sources:
        problems.append("forbidden configured source paths: " + ", ".join(bad_sources))
    strings = output("strings", str(args.binary))
    symbols = output("nm", "-a", str(args.binary))
    if "/Users/" + "dani" in strings or "/home/" in strings:
        problems.append("binary embeds an absolute owner/home path")
    bad = [token for token in FORBIDDEN if token in strings or token in symbols]
    if bad:
        problems.append("forbidden F7/P3 symbols or strings: " + ", ".join(bad))
    for needle in ("presentation=direct GPU", "continuous CPU readback=disabled",
                   "F7/P3 linked=no", "input/event owner=SDL 2.32.10"):
        if needle not in strings:
            problems.append(f"product startup diagnostic missing {needle!r}")
    links = output("ninja", "-C", str(args.build_dir), "-t", "commands", "rrv-product")
    if any(token in links for token in FORBIDDEN):
        problems.append("product link commands include a forbidden renderer source")
    head = output("git", "-C", str(args.producer), "rev-parse", "HEAD").strip()
    tree = output("git", "-C", str(args.producer), "rev-parse", "HEAD^{tree}").strip()
    dirty = output("git", "-C", str(args.producer), "status", "--porcelain=v1").strip()
    if head != args.expected_producer_commit or tree != args.expected_producer_tree or dirty:
        problems.append("producer HEAD/tree/clean state does not match the lock")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=pathlib.Path)
    parser.add_argument("--build-dir", type=pathlib.Path)
    parser.add_argument("--binary", type=pathlib.Path)
    parser.add_argument("--producer", type=pathlib.Path)
    parser.add_argument("--expected-producer-commit")
    parser.add_argument("--expected-producer-tree")
    args = parser.parse_args()
    if args.source_root:
        if any((args.build_dir, args.binary, args.producer, args.expected_producer_commit,
                args.expected_producer_tree)):
            parser.error("--source-root is a standalone source-only check")
        problems = source_gate(args.source_root)
    else:
        if not all((args.build_dir, args.binary, args.producer, args.expected_producer_commit,
                    args.expected_producer_tree)):
            parser.error("binary mode requires build, binary, producer and expected identity arguments")
        problems = source_gate(pathlib.Path(__file__).resolve().parent.parent)
        problems.extend(binary_gate(args))
    if problems:
        print("rrv-product direct-present gate failed:", *problems, sep="\n  ", file=sys.stderr)
        return 1
    print("rrv-product SDL/direct-Metal/no-F7 gate passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
