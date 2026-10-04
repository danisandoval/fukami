#!/usr/bin/env python3
"""Audit the product's actual Ninja closure, compiler includes and Mach-O symbols."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

# Check exact C ABI names, not arbitrary prose (the source may document removal).
RAYLIB_SYMBOLS = re.compile(
    r"^(?:InitWindow|CloseWindow|WindowShouldClose|SetWindowTitle|SetTargetFPS|"
    r"BeginDrawing|EndDrawing|DrawTexturePro|UpdateTexture|LoadTextureFromImage|"
    r"UnloadTexture|InitAudioDevice|CloseAudioDevice|IsAudioDeviceReady|"
    r"LoadWaveFromMemory|UnloadWave|LoadSoundFromWave|UnloadSound|PlaySound|"
    r"StopSound|IsSoundPlaying|SetSoundPitch|SetSoundVolume|IsKeyDown|"
    r"IsGamepadAvailable|GetGamepadAxisMovement|IsGamepadButtonDown|"
    r"rl[A-Z]\w*|glfw\w*|ma_device_\w*|ma_context_\w*)$")
RAYLIB_PATH = re.compile(r"(?:^|[/\\\s\";])(?:lib)?raylib(?:-(?:src|build))?(?:[/\\.\s\";]|$)|"
                         r"(?:^|[/\\])(?:rlgl|raymath)\.h(?:$|\s)", re.I)


def run(*args: str) -> str:
    return subprocess.run(args, check=True, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE).stdout


def graph_objects(graph: str) -> list[str]:
    # Ninja quotes DOT labels using JSON-compatible escaping.
    labels = [json.loads('"' + value + '"')
              for value in re.findall(r'\[label="((?:\\.|[^"\\])*)"', graph)]
    return sorted({label for label in labels if label.endswith('.o')})


def forbidden_symbols(symbols: str) -> list[str]:
    found = set()
    for line in symbols.splitlines():
        fields = line.split()
        if fields:
            symbol = fields[-1].removeprefix('_')
            if RAYLIB_SYMBOLS.fullmatch(symbol):
                found.add(symbol)
    return sorted(found)


def audit(build: Path, binary: Path) -> dict:
    graph = run('ninja', '-C', str(build), '-t', 'graph', 'rrv-product')
    commands = run('ninja', '-C', str(build), '-t', 'commands', 'rrv-product')
    objects = graph_objects(graph)
    problems = []
    if not objects:
        problems.append('product graph contains no auditable object files')
    for label, data in [('dependency graph', graph), ('compile/link commands', commands)]:
        if RAYLIB_PATH.search(data):
            problems.append(f'raylib dependency in product {label}')
    headers = set()
    # Query only the product closure; unrelated diagnostic targets are permitted.
    for start in range(0, len(objects), 64):
        chunk = objects[start:start + 64]
        deps = run('ninja', '-C', str(build), '-t', 'deps', *chunk)
        for obj in chunk:
            if not re.search(r'^' + re.escape(obj) + r': #deps \d+, deps mtime \d+ \(VALID\)$', deps, re.M):
                problems.append(f'missing/stale compiler dependency record: {obj}')
        headers.update(line.strip() for line in deps.splitlines() if line.startswith('    '))
    forbidden_headers = sorted(path for path in headers if RAYLIB_PATH.search(path))
    if forbidden_headers:
        problems.append('raylib compiler includes: ' + ', '.join(forbidden_headers))
    symbols = run('nm', '-a', str(binary))
    forbidden = forbidden_symbols(symbols)
    if forbidden:
        problems.append('raylib/GLFW/miniaudio symbols: ' + ', '.join(forbidden))
    libraries = run('otool', '-L', str(binary))
    if RAYLIB_PATH.search(libraries):
        problems.append('Mach-O loads raylib')
    receipt = {
        'schema': 1, 'target': 'rrv-product',
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'object_count': len(objects), 'compiler_dependency_count': len(headers),
        'object_inventory_sha256': hashlib.sha256('\n'.join(objects).encode()).hexdigest(),
        'commands_sha256': hashlib.sha256(commands.encode()).hexdigest(),
        'forbidden_headers': forbidden_headers, 'forbidden_symbols': forbidden,
        'dynamic_libraries': libraries.splitlines()[1:], 'problems': problems,
    }
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    receipt = audit(args.build_dir.resolve(), args.binary.resolve())
    if args.receipt:
        args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
    if receipt['problems']:
        print('Product raylib exclusion failed:', *receipt['problems'], sep='\n  ')
        return 1
    print(f"Product raylib exclusion passed: {receipt['object_count']} objects, "
          f"{receipt['compiler_dependency_count']} compiler dependencies; no raylib headers, links or symbols")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
