#!/usr/bin/env python3
"""Compose the opt-in GS control candidate after the qualified product overlays.

The locked producer is read-only. Final inputs must exactly equal the ordinary
SDL/SPR source transformations; intrusive diagnostic variants are rejected.
The receipt records every copied/transformed file and transformation source.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from m2_dma_overlay import require_locked_clean_producer
from product_host_overlay import runtime as product_runtime, runtime_header as product_runtime_header
from spr_pending_chain_overlay import apply_patch, PATCH
import gs_control_memory_overlay as memory
import gs_control_gif_overlay as gif
import gs_control_runtime_overlay as runtime
import callback_stack_main_reservation_overlay as callback_stack


SOURCE_FILES = {
    "runtime": "src/lib/ps2_runtime.cpp",
    "memory": "src/lib/ps2_memory.cpp",
    "vif1": "src/lib/ps2_vif1_interpreter.cpp",
    "gif": "src/lib/ps2_gif_arbiter.cpp",
    "interrupt": "src/lib/Kernel/Syscalls/Interrupt.cpp",
    "system": "src/lib/Kernel/Syscalls/System.cpp",
    "stubs_gs": "src/lib/Kernel/Stubs/GS.cpp",
}


def store_image_cpp(text: str) -> str:
    """Make the copied StoreImage HLE match the native download contract.

    Upload sizing remains owned by the producer's bytesForPixels helper.  This
    deliberately replaces only the local-to-host HLE, where the pinned PCSX2
    reader returns packed native bytes rather than the producer's historical
    32-bit-lane approximation for several PSMs.
    """
    start = text.index("    void sceGsExecStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)\n")
    end = text.index("\n    // RRV_CRTC_DIAG", start)
    replacement = '''    void sceGsExecStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t imgAddr = getRegU32(ctx, 4);
        uint32_t dstAddr = getRegU32(ctx, 5);

        GsImageMem img{};
        if (!runtime || !readGsImage(rdram, imgAddr, img))
        {
            setReturnS32(ctx, -1);
            return;
        }

        // StoreImage transfers native packed local-to-host bytes.  Do not use
        // bytesForPixels here: that producer helper intentionally preserves
        // upload lane semantics for CT24/T8H/T4H and is therefore wrong for
        // the PCSX2 GS download FIFO.
        const uint32_t width = img.width;
        const uint32_t height = img.height;
        uint64_t bytes = 0u;
        if (width == 0u || height == 0u || width > 0x0fffu || height > 0x0fffu ||
            img.x > 0x07ffu || img.y > 0x07ffu || img.vram_addr > 0x07ffu)
        {
            setReturnS32(ctx, -1);
            return;
        }
        const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        switch (img.psm)
        {
        case 0: // PSMCT32
            bytes = pixels * 4u;
            break;
        case 1: // PSMCT24
            bytes = pixels * 3u;
            break;
        case 2:  // PSMCT16
        case 10: // PSMCT16S
            bytes = pixels * 2u;
            break;
        case 19: // PSMT8
        case 27: // PSMT8H
            bytes = pixels;
            break;
        case 36: // PSMT4HL
        case 44: // PSMT4HH
            bytes = (pixels + 1u) / 2u;
            break;
        case 20: // PSMT4: pinned native reader has no deterministic FIFO path.
        default:
            setReturnS32(ctx, -1);
            return;
        }

        // The GS descriptor fields and the native bridge's local-memory API
        // are bounded before any temporary packet allocation or GIF DMA.
        constexpr uint64_t kNativeLocalMemoryBytes = 4u * 1024u * 1024u;
        const uint32_t fbw = img.vram_width ? img.vram_width : (width + 63u) / 64u;
        if (fbw == 0u || fbw > 0x3fu || bytes == 0u || bytes > kNativeLocalMemoryBytes ||
            bytes > 0xffffffffu || dstAddr >= PS2_RAM_SIZE ||
            bytes > static_cast<uint64_t>(PS2_RAM_SIZE - dstAddr))
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint8_t *dst = getMemPtr(rdram, dstAddr);
        if (!dst)
        {
            setReturnS32(ctx, -1);
            return;
        }

        // Admit the actual native result operation before temporary packet
        // allocation, GIF DMA register writes, or pending-transfer service.
        // The runtime owns the sole G1-A classification and diagnostic, and
        // this has no side effects on retained producer work.
        runtime->preflightActiveGsBackendLocalMemoryRead();
        if (!runtime->syncCoreSubsystems())
        {
            setReturnS32(ctx, -1);
            return;
        }

        const uint32_t totalImageBytes = static_cast<uint32_t>(bytes);
        uint32_t sbp = (static_cast<uint32_t>(img.vram_addr) * 2048u) / 256u;
        uint64_t bitbltbuf = (static_cast<uint64_t>(sbp & 0x3fffu) << 0) |
                             (static_cast<uint64_t>(fbw & 0x3fu) << 16) |
                             (static_cast<uint64_t>(img.psm & 0x3fu) << 24) |
                             (static_cast<uint64_t>(0u) << 32) |
                             (static_cast<uint64_t>(1u) << 48) |
                             (static_cast<uint64_t>(0u) << 56);
        uint64_t trxpos = (static_cast<uint64_t>(img.x) << 0) |
                          (static_cast<uint64_t>(img.y) << 16) |
                          (static_cast<uint64_t>(0u) << 32) |
                          (static_cast<uint64_t>(0u) << 48);
        uint64_t trxreg = static_cast<uint64_t>(height) << 32 | static_cast<uint64_t>(width);

        uint32_t pktAddr = runtime->guestMalloc(80u, 16u);
        if (pktAddr == 0u)
        {
            setReturnS32(ctx, -1);
            return;
        }
        struct GuestPacket final
        {
            PS2Runtime *runtime;
            uint32_t address;
            ~GuestPacket() { runtime->guestFree(address); }
        } packet{runtime, pktAddr};

        uint8_t *pkt = getMemPtr(rdram, pktAddr);
        if (!pkt)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint64_t *q = reinterpret_cast<uint64_t *>(pkt);
        q[0] = makeGiftagAplusD(4u);
        q[1] = 0xEULL;
        q[2] = bitbltbuf;
        q[3] = 0x50ULL;
        q[4] = trxpos;
        q[5] = 0x51ULL;
        q[6] = trxreg;
        q[7] = 0x52ULL;
        q[8] = 1ULL;
        q[9] = 0x53ULL;

        constexpr uint32_t GIF_CHANNEL = 0x1000A000;
        constexpr uint32_t CHCR_STR_MODE0 = 0x101u;
        auto &mem = runtime->memory();
        mem.writeIORegister(GIF_CHANNEL + 0x10u, pktAddr);
        mem.writeIORegister(GIF_CHANNEL + 0x20u, 5u);
        mem.writeIORegister(GIF_CHANNEL + 0x00u, CHCR_STR_MODE0);
        mem.processPendingTransfers();

        // Do not fall back to the retired producer-local GS reader.  The
        // PCSX2 bridge owns m_tr and its result boundary; a false result is a
        // fail-closed HLE result while bridge exceptions retain their native
        // propagation semantics.
        if (!runtime->readActiveGsBackendLocalMemory(dst, totalImageBytes, bitbltbuf, trxpos, trxreg))
        {
            setReturnS32(ctx, -1);
            return;
        }
        setReturnS32(ctx, 0);
    }
'''
    return text[:start] + replacement + text[end:]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def store(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def generate(source: Path, include_input: Path, inputs: dict[str, Path], output: Path,
             callback_stack_main_reservation: bool = False) -> None:
    source, include_input, output = source.resolve(), include_input.resolve(), output.resolve()
    require_locked_clean_producer(source)
    if source == output or source in output.parents or output in source.parents:
        raise RuntimeError("GS control overlay must be outside the locked producer")
    if include_input == output or output in include_input.parents or include_input in output.parents:
        raise RuntimeError("GS control header output must be separate from its input")
    callback_outputs: dict[str, bytes] = {}
    if callback_stack_main_reservation:
        callback_inputs = {
            callback_stack.HEADER: product_runtime_header(
                (source / callback_stack.HEADER).read_text()).encode(),
            callback_stack.RUNTIME: product_runtime(
                (source / callback_stack.RUNTIME).read_text()).encode(),
        }
        for name in callback_stack.CANONICAL_FILES:
            callback_inputs[name] = (source / name).read_bytes()
        callback_outputs = callback_stack.apply_patch(callback_inputs)
    files = []

    def emit(name: str, path: Path, raw: bytes, result: bytes) -> None:
        destination = output / name
        if destination.resolve() == path.resolve():
            raise RuntimeError("GS control overlay cannot overwrite an input")
        store(destination, result)
        files.append({"path": name, "input_path": str(path.resolve()),
                      "producer_sha256": sha((source / name).read_bytes()),
                      "input_sha256": sha(raw), "overlay_sha256": sha(result)})

    # Copy the entire include closure so quoted includes within ps2_runtime.h
    # cannot silently resolve the old ps2_memory/GIF object layouts.
    for original in sorted((source / "include").rglob("*")):
        if not original.is_file():
            continue
        name = str(original.relative_to(source))
        path = include_input / original.relative_to(source / "include")
        raw = path.read_bytes()
        expected = original.read_bytes()
        if name == "include/ps2_host_backend.h":
            expected = b"#pragma once\n// Product host interfaces are explicit; no diagnostic host API.\n"
        elif name == "include/ps2_runtime.h":
            # Product host owns the typed terminal-outcome member/accessor.
            # The control overlay must retain that complete runtime layout;
            # accepting the raw producer header here would split outer TUs.
            expected = (callback_outputs[name] if callback_stack_main_reservation
                        else product_runtime_header(expected.decode()).encode())
        if raw != expected:
            raise RuntimeError(f"GS control rejects non-ordinary product header: {name}")
        text = raw.decode()
        if name == "include/runtime/ps2_memory.h":
            text = runtime.memory_irq_header(memory.memory_header(text))
        elif name == "include/runtime/ps2_gif_arbiter.h":
            text = gif.gif_header(text)
        emit(name, path, raw, text.encode())

    for key, name in SOURCE_FILES.items():
        path = inputs[key].resolve()
        raw = path.read_bytes()
        original = (source / name).read_bytes()
        expected = original
        if key == "runtime":
            expected = product_runtime(original.decode()).encode()
        elif key == "memory":
            expected = apply_patch(original, PATCH.read_bytes())
        if callback_stack_main_reservation and name in callback_outputs:
            expected = callback_outputs[name]
        if raw != expected:
            raise RuntimeError(f"GS control rejects unknown/intrusive final input: {name}")
        transform = {
            "runtime": runtime.runtime_cpp,
            "memory": lambda text: runtime.memory_irq_cpp(memory.memory_cpp(text)),
            "vif1": lambda text: runtime.vif_irq_cpp(memory.vif1_cpp(text)),
            "gif": gif.gif_cpp,
            "interrupt": runtime.interrupt_cpp,
            "system": runtime.system_cpp,
            "stubs_gs": store_image_cpp,
        }[key]
        emit(name, path, raw, transform(raw.decode()).encode())

    name = "src/lib/Kernel/Syscalls/Interrupt.h"
    path = source / name
    raw = path.read_bytes()
    emit(name, path, raw, runtime.interrupt_header(raw.decode()).encode())
    root = Path(__file__).resolve().parents[1]
    transforms = [Path(__file__), Path(memory.__file__), Path(gif.__file__), Path(runtime.__file__),
                  root / "src/gs-control/rrv_gs_control.h", root / "src/gs-control/rrv_gs_result_boundary.h",
                  root / "src/gs-control/rrv_gif_control_stream.h"]
    receipt = {"schema_version": 1, "overlay": "opt-in-producer-gs-control",
               "renderer_execution": "synchronous-only", "files": files,
               "transforms": [{"path": str(path.relative_to(root)), "sha256": sha(path.read_bytes())}
                              for path in transforms]}
    store(output / "gs-control-overlay-manifest.json",
          (json.dumps(receipt, sort_keys=True, indent=2) + "\n").encode())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-source", type=Path, required=True)
    parser.add_argument("--include-input", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--callback-stack-main-reservation", action="store_true")
    for key in SOURCE_FILES:
        parser.add_argument(f"--{key}-input", type=Path, required=True)
    args = parser.parse_args()
    generate(args.runtime_source, args.include_input,
             {key: getattr(args, key + "_input") for key in SOURCE_FILES}, args.output_dir,
             args.callback_stack_main_reservation)


if __name__ == "__main__":
    main()
