#!/usr/bin/env python3
# retired from the source-owned product 2026-10-01 (third_party/ps2recomp + src/product hold its effect);
# still used by the legacy/diagnostic configurations (RRV_PRODUCT_OWNED_SOURCE=OFF) through cmake/Rrv*.cmake.
"""Build-local SDL product host specialization of the immutable producer.

The diagnostic producer remains untouched. Whole-file pins protect the base;
unique semantic anchors protect composition after optional diagnostic overlays.
Every copied input and resulting file is recorded in the build receipt.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

from m2_causal_overlay import replace_once
from m2_dma_overlay import require_locked_clean_producer

PINNED = {
    "CMakeLists.txt": "cf6208ba816e9ea657c31f93289d925319ae24e62a93d3bcd3e2af4731d3ebf6",
    "include/ps2_host_backend.h": "b0b4bfa2e3b780a23cababa2c5b4a8e6909be65f0417ceb5c591a4e68cbc66bf",
    "include/ps2_runtime.h": "3e6ce80b20c3675a326843289d34785034499799501cc36d812504ba83e0add3",
    "src/lib/ps2_runtime.cpp": "83bce4c13f3fdb34585ca32e5126c4db43516612860df18297e99ad35469dab3",
    "src/lib/ps2_pad.cpp": "ea73159c79bda2f00a59cd52c03de1dfe553d27a6dd8c27fc140e2606be0e7ee",
    "src/lib/ps2_audio.cpp": "00baeae2f761812539297bce306e299c49ff94857f4ff12271d1bfd604d469c9",
    "src/lib/Kernel/Syscalls/Thread.cpp": "f22e57b9b75c40fe69014194dca91ad7a9af2a93d7cb1cd3719a662603b9d3b8",
}
MARKER = "// RRV product SDL host overlay\n"


def region(text: str, begin: str, end: str, replacement: str, label: str) -> str:
    if text.count(begin) != 1 or text.count(end) != 1:
        raise RuntimeError(f"{label}: expected unique begin/end anchors")
    start = text.index(begin)
    finish = text.index(end, start)
    return text[:start] + replacement + text[finish:]


def runtime(text: str) -> str:
    text = replace_once(text, '#include "ps2_host_backend.h"', '#include "rrv_sdl_audio.h"', 'runtime host include')
    text = region(text, '    bool legacyHeadlessDiagnosticEnabled()\n', '\n}\n\nPS2Runtime::BackEdgeYieldSuppressionScope::BackEdgeYieldSuppressionScope()', '', 'legacy texture helper')
    text = region(text, 'static void UploadFrame(', 'void PS2Runtime::dispatchGsPacket(', '', 'legacy framebuffer upload')
    text = region(text, '#if defined(PLATFORM_VITA)\n        m_audioBackend.stopAll();', '        m_loadedModules.clear();',
                  '        m_audioBackend.stopAll();\n        rrv::audio::shutdown();\n        m_audioBackend.setAudioReady(false);\n\n', 'runtime shutdown')
    text = region(text, '        const bool directPresentation = directGpuPresentationActive();\n', '\n        return true;\n    }\n    catch (const std::exception &e)\n    {\n        std::cerr << "Failed to initialize PS2 runtime: "',
                  '        if (!directGpuPresentationActive())\n'
                  '            throw std::logic_error("product runtime requires SDL direct presentation");\n'
                  '        rrv::audio::initialize();\n'
                  '        m_audioBackend.setAudioReady(rrv::audio::ready());\n'
                  '        std::fprintf(stderr, "[m1-runtime] SDL direct GPU presentation enabled; "\n'
                  '                     "SDL audio-ready=%s\\n", rrv::audio::ready() ? "yes" : "no");\n', 'product initialize')
    text = region(text, '    const bool headlessDiagnostic = legacyHeadlessDiagnosticEnabled();\n', '    g_activeThreads.store(1,',
                  '    if (!directGpuPresentationActive())\n'
                  '        throw std::logic_error("product runtime requires SDL direct presentation");\n\n', 'legacy frame texture')
    # The direct branch always continued before this legacy-only loop body.
    text = region(text, '        if (directPresentation)\n', '    }\n    }\n    catch (...)\n    {\n        // UploadFrame()',
                  '        serviceDirectPresentation();\n', 'product host loop')
    text = region(text, '    if (!headlessDiagnostic && !directPresentation)\n', '    const int remainingThreads =', '', 'legacy window cleanup')
    text = replace_once(text, '    auto headlessNextPresent = std::chrono::steady_clock::now();\n', '', 'unused legacy deadline')
    text = replace_once(
        text,
        'void PS2Runtime::run()\n{\n',
        'void PS2Runtime::run()\n{\n'
        '    const auto rrvTerminalOutcome = terminalOutcomeHandle();\n'
        '    rrv::guestoutcome::begin(*rrvTerminalOutcome);\n',
        'terminal outcome run start')
    text = replace_once(
        text,
        'void PS2Runtime::dispatchLoop(uint8_t *rdram, R5900Context *ctx)\n{\n',
        'void PS2Runtime::dispatchLoop(uint8_t *rdram, R5900Context *ctx)\n{\n'
        '    const auto rrvTerminalOutcome = terminalOutcomeHandle();\n',
        'terminal outcome dispatch handle')
    text = replace_once(
        text,
        '        RecompiledFunction fn = lookupFunction(pc);\n',
        '        // Product qualification records a rejected target before the producer\n'
        '        // fallback can attempt diagnostic recovery.  Registered indirect/re-entry\n'
        '        // targets keep their existing dispatch behavior and create no outcome.\n'
        '        if (!hasFunction(pc))\n'
        '        {\n'
        '            rrv::guestoutcome::recordFailure(\n'
        '                *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::RejectedMainDispatch,\n'
        '                {pc, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)),\n'
        '                 static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0))},\n'
        '                "unregistered main dispatch target");\n'
        '        }\n'
        '        RecompiledFunction fn = lookupFunction(pc);\n',
        'terminal outcome main dispatch rejection')
    text = replace_once(
        text,
        '        if (ctx->pc == 0u)\n        {\n            const uint32_t ra =',
        '        if (ctx->pc == 0u)\n        {\n'
        '            rrv::guestoutcome::recordMainCompletion(\n'
        '                *rrvTerminalOutcome,\n'
        '                {ctx->pc, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)),\n'
        '                 static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0))});\n'
        '            const uint32_t ra =',
        'terminal outcome main completion')
    text = replace_once(text, '    std::thread gameThread([&]()\n',
                        '    std::thread gameThread([&, rrvTerminalOutcome]()\n',
                        'terminal outcome game-thread handle capture')
    text = replace_once(
        text,
        '        catch (const std::exception &e)\n        {\n            std::cerr << "Error during program execution: " << e.what() << std::endl;\n        }\n'
        '        catch (...)\n        {\n            std::cerr << "Error during program execution: unknown exception" << std::endl;\n        }\n',
        '        catch (const std::exception &e)\n        {\n'
        '            rrv::guestoutcome::recordFailure(\n'
        '                *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::MainGuestException,\n'
        '                {m_cpuContext.pc, static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)),\n'
        '                 static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0))}, e.what());\n'
        '            requestStop();\n'
        '            std::cerr << "Error during program execution: " << e.what() << std::endl;\n'
        '        }\n'
        '        catch (...)\n'
        '        {\n'
        '            rrv::guestoutcome::recordFailure(\n'
        '                *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::MainGuestException,\n'
        '                {m_cpuContext.pc, static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)),\n'
        '                 static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0))}, "unknown main dispatch exception");\n'
        '            requestStop();\n'
        '            std::cerr << "Error during program execution: unknown exception" << std::endl;\n'
        '        }\n',
        'terminal outcome main exception')
    text = replace_once(
        text,
        '        hostLoopFailure = std::current_exception();\n        requestStop();\n',
        '        hostLoopFailure = std::current_exception();\n'
        '        rrv::guestoutcome::recordFailure(\n'
        '            *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::HostRuntimeException,\n'
        '            {},\n'
        '            "host presentation/runtime exception");\n'
        '        requestStop();\n',
        'terminal outcome host exception')
    text = replace_once(
        text,
        '            std::cerr << "[run] game thread did not stop within timeout; detaching" << std::endl;\n            gameThread.detach();\n',
        '            rrv::guestoutcome::recordFailure(\n'
        '                *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::ShutdownTimeout,\n'
        '                {},\n'
        '                "main guest thread shutdown timeout");\n'
        '            std::cerr << "[run] game thread did not stop within timeout; detaching" << std::endl;\n            gameThread.detach();\n',
        'terminal outcome main shutdown timeout')
    text = replace_once(
        text,
        '        std::cerr << "[run] guest host threads did not stop within timeout; detaching remaining worker threads"\n'
        '                  << std::endl;\n        ps2_syscalls::detachAllGuestHostThreads();\n',
        '        rrv::guestoutcome::recordFailure(\n'
        '            *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::ShutdownTimeout,\n'
        '            {},\n'
        '            "guest worker shutdown timeout");\n'
        '        std::cerr << "[run] guest host threads did not stop within timeout; detaching remaining worker threads"\n'
        '                  << std::endl;\n        ps2_syscalls::detachAllGuestHostThreads();\n',
        'terminal outcome worker shutdown timeout')
    text = replace_once(
        text,
        '    // `gameThread` was joined or detached above, so this preserves the first\n',
        '    rrv::guestoutcome::markRunFinished(*rrvTerminalOutcome);\n\n'
        '    // `gameThread` was joined or detached above, so this preserves the first\n',
        'terminal outcome run finish')
    return MARKER + text


def runtime_header(text: str) -> str:
    text = replace_once(text, '#include "runtime/ps2_pad.h"',
                        '#include "runtime/ps2_pad.h"\n#include "rrv_guest_terminal_outcome.h"',
                        'terminal outcome header include')
    text = replace_once(
        text,
        '    inline const PSPadBackend &padBackend() const { return m_padBackend; }\n',
        '    inline const PSPadBackend &padBackend() const { return m_padBackend; }\n'
        '    inline rrv::guestoutcome::RuntimeTerminalOutcomeHandle terminalOutcomeHandle() const\n'
        '    { return m_rrvTerminalOutcome; }\n',
        'terminal outcome accessors')
    text = replace_once(
        text,
        '    bool readActiveGsBackendLocalMemory(uint8_t *dst, uint32_t byteCount,\n'
        '                                        uint64_t bitbltbuf, uint64_t trxpos,\n'
        '                                        uint64_t trxreg);\n',
        '    bool readActiveGsBackendLocalMemory(uint8_t *dst, uint32_t byteCount,\n'
        '                                        uint64_t bitbltbuf, uint64_t trxpos,\n'
        '                                        uint64_t trxreg);\n'
        '    // Result-boundary admission shared by direct reads and HLEs before\n'
        '    // they can submit a native local-to-host transaction.\n'
        '    void preflightActiveGsBackendLocalMemoryRead();\n',
        'GS local-memory result preflight declaration')
    text = replace_once(
        text,
        '    R5900Context m_cpuContext;\n',
        '    R5900Context m_cpuContext;\n'
        '    rrv::guestoutcome::RuntimeTerminalOutcomeHandle m_rrvTerminalOutcome =\n'
        '        rrv::guestoutcome::makeRuntimeTerminalOutcome();\n',
        'terminal outcome runtime ownership')
    return MARKER + text


def thread_syscall(text: str) -> str:
    text = replace_once(text, '#include "rrv_snapshot_hooks.h" // developer snapshots (docs/SNAPSHOTS.md)',
                        '#include "rrv_snapshot_hooks.h" // developer snapshots (docs/SNAPSHOTS.md)\n'
                        '#include "rrv_guest_terminal_outcome.h"',
                        'terminal outcome worker include')
    text = replace_once(
        text,
        '        if (!runtime || !runtime->hasFunction(info->entry))\n'
        '        {\n'
        '            std::cerr << "[StartThread] entry 0x" << std::hex << info->entry << std::dec << " is not registered" << std::endl;\n'
        '            setReturnS32(ctx, KE_ERROR);\n'
        '            return;\n'
        '        }\n',
        '        if (!runtime || !runtime->hasFunction(info->entry))\n'
        '        {\n'
        '            if (runtime)\n'
        '            {\n'
        '                const auto rrvTerminalOutcome = runtime->terminalOutcomeHandle();\n'
        '                if (rrvTerminalOutcome)\n'
        '                {\n'
        '                    rrv::guestoutcome::recordFailure(\n'
        '                        *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::RejectedWorkerDispatch,\n'
        '                        {info->entry, GPR_U32(ctx, 31), GPR_U32(ctx, 29)},\n'
        '                        "unregistered StartThread initial entry");\n'
        '                }\n'
        '                runtime->requestStop();\n'
        '            }\n'
        '            setReturnS32(ctx, KE_ERROR);\n'
        '            return;\n'
        '        }\n',
        'terminal outcome initial worker rejection')
    text = replace_once(
        text,
        '        g_activeThreads.fetch_add(1, std::memory_order_relaxed);\n',
        '        const auto rrvTerminalOutcome = runtime->terminalOutcomeHandle();\n'
        '        g_activeThreads.fetch_add(1, std::memory_order_relaxed);\n',
        'terminal outcome worker handle capture')
    text = replace_once(
        text,
        '                    PS2Runtime::RecompiledFunction step = runtime->lookupFunction(pc);\n'
        '                    if (!step)\n                    {\n'
        '                        std::cerr << "[StartThread] id=" << tid << " missing function for pc=0x"\n'
        '                                  << std::hex << pc << std::dec << std::endl;\n'
        '                        throw ThreadExitException();\n'
        '                    }\n',
        '                    if (!runtime->hasFunction(pc))\n'
        '                    {\n'
        '                        rrv::guestoutcome::recordFailure(\n'
        '                            *rrvTerminalOutcome,\n'
        '                            rrv::guestoutcome::TerminalKind::RejectedWorkerDispatch,\n'
        '                            {pc, GPR_U32(threadCtx, 31), GPR_U32(threadCtx, 29)},\n'
        '                            "unregistered StartThread later dispatch target");\n'
        '                        runtime->requestStop();\n'
        '                        std::cerr << "[StartThread] id=" << tid << " missing function for pc=0x"\n'
        '                                  << std::hex << pc << std::dec << std::endl;\n'
        '                        throw ThreadExitException();\n'
        '                    }\n'
        '                    PS2Runtime::RecompiledFunction step = runtime->lookupFunction(pc);\n'
        '                    if (!step)\n'
        '                    {\n'
        '                        rrv::guestoutcome::recordFailure(\n'
        '                            *rrvTerminalOutcome,\n'
        '                            rrv::guestoutcome::TerminalKind::WorkerGuestException,\n'
        '                            {pc, GPR_U32(threadCtx, 31), GPR_U32(threadCtx, 29)},\n'
        '                            "registered StartThread dispatch had no callable step");\n'
        '                        runtime->requestStop();\n'
        '                        throw ThreadExitException();\n'
        '                    }\n',
        'terminal outcome worker dispatch rejection')
    text = replace_once(
        text,
        '            catch (const std::exception &e)\n            {\n'
        '                std::cerr << "[StartThread] id=" << tid << " exception: " << e.what() << std::endl;\n'
        '            }\n',
        '            catch (const std::exception &e)\n            {\n'
        '                rrv::guestoutcome::recordFailure(\n'
        '                    *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::WorkerGuestException,\n'
        '                    {threadCtx->pc, GPR_U32(threadCtx, 31), GPR_U32(threadCtx, 29)}, e.what());\n'
        '                runtime->requestStop();\n'
        '                std::cerr << "[StartThread] id=" << tid << " exception: " << e.what() << std::endl;\n'
        '            }\n',
        'terminal outcome worker exception')
    return MARKER + text


def pad(text: str) -> str:
    text = replace_once(text, '#include "ps2_host_backend.h"\n', '', 'pad host include')
    text = region(text, 'class RaylibHostPadBackend final', 'enum class BackendMode', '', 'legacy pad classes')
    text = region(text, '        if (!neutralTest && !external)\n', '        if (diagnostics)\n',
                  '        // SDL owns product input; absent injection remains disconnected.\n'
                  '        // Preserve the existing late-injection seam used by runtime tests.\n', 'external SDL owner')
    return MARKER + text


def audio(text: str) -> str:
    text = replace_once(text, '#include "ps2_host_backend.h"', '#include "rrv_sdl_audio.h"', 'audio host include')
    text = region(text, 'namespace\n{\n    std::vector<uint8_t> buildWavFromPcm', 'namespace ps2_vag', '', 'redundant PCM WAV wrapping')
    text = replace_once(text, '        Sound snd;', '        rrv::audio::SoundHandle snd;', 'owned PCM sound')
    text = region(text, '    std::vector<uint8_t> wav = buildWavFromPcm', '    m_impl->activeSounds.push_back({snd, sampleKey});',
                  '    auto snd = rrv::audio::createMonoSound(sample.pcm.data(), sample.pcm.size(),\n'
                  '                                            sample.sampleRate, pitch, volume);\n'
                  '    if (!snd)\n        return;\n', 'SDL PCM sound creation')
    for old, new in [('IsSoundPlaying(', 'rrv::audio::playing('), ('StopSound(', 'rrv::audio::stop('), ('PlaySound(', 'rrv::audio::play(')]:
        if old not in text:
            raise RuntimeError(f'audio missing {old}')
        text = text.replace(old, new)
    text = re.sub(r'^[ \t]*UnloadSound\([^\n]+\);\n', '', text, flags=re.M)
    return MARKER + text


def cmake(text: str, source: Path) -> str:
    # Retain the producer source inventory and field-only include boundary.
    sources = text[text.index('add_library(ps2_runtime STATIC\n'):text.index('\nif(APPLE AND NOT PS2X_IS_VITA)')]
    sources = re.sub(r'(?m)^    (src/[^\n]+)$', lambda m: '    "' + str(source / m.group(1)) + '"', sources)
    sources = sources.replace(str(source / 'src/lib/ps2_audio.cpp'), '${CMAKE_CURRENT_SOURCE_DIR}/src/lib/ps2_audio.cpp')
    field = text[text.index('if(PS2X_RRV_FIELD_ONLY)\n'):text.index('\ntarget_link_libraries(ps2_runtime PUBLIC')]
    return (MARKER.replace('//', '#') +
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(RrvProductRuntime LANGUAGES CXX)\n'
        'set(CMAKE_CXX_STANDARD 20)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\n'
        'if(NOT PS2X_RRV_FIELD_ONLY OR NOT TARGET rrv_sdl_product_audio)\n'
        '    message(FATAL_ERROR "SDL product runtime requires the field backend and product audio target")\nendif()\n' +
        sources + '\n'
        f'file(GLOB_RECURSE KERNEL_SRC_FILES CONFIGURE_DEPENDS "{source}/src/lib/Kernel/*.cpp")\n'
        f'list(REMOVE_ITEM KERNEL_SRC_FILES "{source}/src/lib/Kernel/Syscalls/Thread.cpp")\n'
        'target_sources(ps2_runtime PRIVATE ${KERNEL_SRC_FILES}\n'
        '    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel/Syscalls/Thread.cpp")\n'
        f'set_source_files_properties("{source}/src/lib/ps2_vu1.cpp" PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")\n'
        'target_include_directories(ps2_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"\n'
        '    "${CMAKE_SOURCE_DIR}/src"\n'
        f'    "{source}/src/lib/Kernel"\n'
        f'    "{source}/src/lib/Kernel/Syscalls")\n' + field + '\n'
        'target_link_libraries(ps2_runtime PUBLIC rrv_sdl_product_audio)\n'
        f'include("{source}/cmake/ReleaseMode.cmake")\n'
        'if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")\n'
        '    EnableFastReleaseMode(ps2_runtime)\nendif()\n')


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def verify(source: Path) -> None:
    require_locked_clean_producer(source)
    for name, expected in PINNED.items():
        if sha((source / name).read_bytes()) != expected:
            raise RuntimeError(f'product host overlay requires pinned {name}')


def receipt(output: Path, name: str, files: list[dict]) -> None:
    (output / name).write_text(json.dumps({'schema_version': 1, 'overlay': 'product-sdl-host', 'files': files}, indent=2, sort_keys=True) + '\n')


def generate(source: Path, output: Path) -> None:
    verify(source)
    source, output = source.resolve(), output.resolve()
    if output == source or source in output.parents or output in source.parents:
        raise RuntimeError('product host overlay must be outside the immutable producer')
    # Only headers and CMake support need copying. The CMake source inventory
    # deliberately retains original paths for subsequent diagnostic overlays.
    shutil.copytree(source / 'include', output / 'include', dirs_exist_ok=True)
    changes = {'CMakeLists.txt': cmake((source / 'CMakeLists.txt').read_text(), source),
               'include/ps2_host_backend.h': '#pragma once\n// Product host interfaces are explicit; no diagnostic host API.\n',
               'include/ps2_runtime.h': runtime_header((source / 'include/ps2_runtime.h').read_text()),
               'src/lib/Kernel/Syscalls/Thread.cpp': thread_syscall(
                   (source / 'src/lib/Kernel/Syscalls/Thread.cpp').read_text()),
               'src/lib/ps2_audio.cpp': audio((source / 'src/lib/ps2_audio.cpp').read_text())}
    for name, value in changes.items():
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(value)
    files = [{'path': str(path.relative_to(output)),
              'producer_sha256': sha((source / path.relative_to(output)).read_bytes()),
              'overlay_sha256': sha(path.read_bytes())}
             for path in sorted(output.rglob('*')) if path.is_file() and path.name != 'product-host-overlay-manifest.json']
    receipt(output, 'product-host-overlay-manifest.json', files)


def known_runtime_inputs(original: str) -> dict[str, str]:
    from m2_causal_overlay import patch_runtime, patch_initial_state
    from m2_dma_overlay import runtime as dma_runtime
    from m2_guest_overlay import patch_runtime as guest_runtime
    from m2p_game001_overlay import patch_runtime as fail_closed_runtime
    causal = patch_runtime(original)[0]
    dma = dma_runtime(causal)
    guest = guest_runtime(dma)
    variants = {"locked-producer": original, "causal": causal,
                "causal-dma": dma, "causal-dma-guest": guest,
                "game001-fail-closed": fail_closed_runtime(original)[0]}
    for name, value in list(variants.items()):
        if name.startswith("causal"):
            variants[name + "-initial-state"] = patch_initial_state(value)
    return variants


def finalize(source: Path, runtime_source: Path, pad_source: Path, output: Path) -> None:
    verify(source)
    output = output.resolve()
    if output == source.resolve() or source.resolve() in output.parents or output in source.resolve().parents:
        raise RuntimeError('product host final overlay must be outside the immutable producer')
    files = []
    for name, path, transform in [('ps2_runtime.cpp', runtime_source, runtime), ('ps2_pad.cpp', pad_source, pad)]:
        raw = path.read_bytes()
        text = raw.decode()
        if text.startswith(MARKER):
            raise RuntimeError('product host finalization must compose before a prior host specialization')
        original = (source / 'src/lib' / name).read_text()
        candidates = known_runtime_inputs(original) if name == 'ps2_runtime.cpp' else {'locked-producer': original}
        matched = [kind for kind, candidate in candidates.items() if text == candidate]
        if len(matched) != 1:
            raise RuntimeError(f'product host rejects unknown input transformation: {name}')
        result = transform(text).encode()
        target = output / 'src/lib' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(result)
        files.append({'path': 'src/lib/' + name, 'input_path': str(path),
                      'producer_sha256': PINNED['src/lib/' + name],
                      'input_kind': matched[0], 'input_sha256': sha(raw), 'overlay_sha256': sha(result)})
    # DMA/guest diagnostic overlays copy the whole producer include closure.
    # Override just this host marker, ahead of those coherent ABI headers.
    host_header = output / 'include/ps2_host_backend.h'
    host_header.parent.mkdir(parents=True, exist_ok=True)
    host_header.write_text('#pragma once\n// Product host interfaces are explicit; no diagnostic host API.\n')
    files.append({'path': 'include/ps2_host_backend.h',
                  'producer_sha256': PINNED['include/ps2_host_backend.h'],
                  'overlay_sha256': sha(host_header.read_bytes())})
    receipt(output, 'product-host-final-manifest.json', files)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-source', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--runtime-input', type=Path)
    parser.add_argument('--pad-input', type=Path)
    args = parser.parse_args()
    if args.runtime_input or args.pad_input:
        if not args.runtime_input or not args.pad_input:
            parser.error('finalization requires both runtime and pad inputs')
        finalize(args.runtime_source, args.runtime_input, args.pad_input, args.output_dir)
    else:
        generate(args.runtime_source, args.output_dir)


if __name__ == '__main__':
    main()
