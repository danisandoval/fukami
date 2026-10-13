"""Compile one C++ translation unit with the host compiler, for tests that build a small helper executable.

`c++` (GNU-style options) on macOS and Linux; clang-cl (MSVC-style options, from a vcvars shell) on Windows.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable


def find_compiler() -> str | None:
    return shutil.which("clang-cl" if sys.platform == "win32" else "c++")


def compile_cpp(source: Path, exe: Path, *, optimize: str = "2", fp_contract_off: bool = False, sse41: bool = False,
                include_dirs: Iterable[Path | str] = (), defines: Iterable[str] = ()) -> Path:
    """Build `source` into an executable and return its path (`exe`, plus `.exe` on Windows)."""
    compiler = find_compiler()
    if compiler is None:
        raise AssertionError("a C++ compiler is required (c++, or clang-cl from a vcvars shell on Windows)")
    if sys.platform == "win32":
        exe = exe.with_suffix(".exe")
        cmd = [compiler, "/nologo", "/std:c++20", "/EHsc", f"/O{optimize}"]
        if fp_contract_off:
            cmd.append("/clang:-ffp-contract=off")
        if sse41:
            cmd.append("/clang:-msse4.1")
        cmd += [f"/I{d}" for d in include_dirs] + [f"/D{d}" for d in defines]
        cmd += [str(source), f"/Fo{exe.parent}\\", f"/Fe{exe}"]
    else:
        cmd = [compiler, "-std=c++20", f"-O{optimize}"]
        if fp_contract_off:
            cmd.append("-ffp-contract=off")
        if sse41:
            cmd.append("-msse4.1")
        cmd += [f"-I{d}" for d in include_dirs] + [f"-D{d}" for d in defines]
        cmd += [str(source), "-o", str(exe)]
    subprocess.run(cmd, check=True)
    return exe
