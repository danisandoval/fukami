#!/usr/bin/env python3
"""Build the third-party libraries PCSX2's CMake build needs on Windows, into one install prefix.

This is the subset of PCSX2's own Windows dependency script that the GS bridge links
(.github/workflows/scripts/windows/build-dependencies.bat of the locked PCSX2 revision, build-deps/pcsx2-2.8.2):
no Qt, KDDockWidgets, FFmpeg or Agility SDK (the bridge is Vulkan-only, has no UI and no video capture). Same library
versions and the same CMake options as that script, built with the MSVC compiler it uses. Sources are downloaded from
the URLs pinned in config/dependencies.lock.toml ([[pcsx2_windows_source]]) and refused unless their SHA-256 matches.

Run from a shell that has run vcvars64 (cl, cmake, ninja and python on PATH; BUILDING.md):

    python scripts/build_pcsx2_windows_deps.py

The prefix defaults to <PCSX2 checkout>/deps (ignored by that repository's .gitignore) and is what
scripts/build_pcsx2_gs_bridge.py passes to CMake as CMAKE_PREFIX_PATH and what cmake/RrvDiscExtract.cmake reads
zlib/zstd from. Finished steps leave a stamp in <prefix>/.rrv-stamps; a rerun skips them (--force rebuilds).

Deviation from the upstream script: libpng is built without the APNG patch (only the Qt UI uses animated PNG).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import zipfile

from prepare_dependencies import load_toml

ROOT = pathlib.Path(__file__).resolve().parent.parent
LOCK = ROOT / "config" / "dependencies.lock.toml"
DEFAULT_PCSX2 = ROOT / "build-deps" / "pcsx2-2.8.2"
# Older third-party CMake projects declare a minimum below what CMake 4 accepts.
POLICY = "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"


class Failure(RuntimeError):
    pass


def sources() -> dict[str, dict]:
    entries = load_toml(LOCK).get("pcsx2_windows_source", [])
    table = {str(entry["name"]): entry for entry in entries}
    if not table:
        raise Failure("dependencies.lock.toml has no [[pcsx2_windows_source]] entries")
    return table


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(entry: dict, cache: pathlib.Path) -> pathlib.Path:
    target = cache / str(entry["filename"])
    expected = str(entry["sha256"]).lower()
    if target.is_file() and sha256(target) == expected:
        return target
    partial = target.with_suffix(target.suffix + ".part")
    print(f"downloading {entry['filename']}")
    subprocess.run(["curl.exe", "--fail", "--location", "--retry", "3", "--silent", "--show-error",
                    "--output", str(partial), str(entry["url"])], check=True)
    actual = sha256(partial)
    if actual != expected:
        partial.unlink()
        raise Failure(f"{entry['filename']}: SHA-256 {actual} differs from the pinned {expected}")
    os.replace(partial, target)
    return target


def extract(archive: pathlib.Path, destination: pathlib.Path) -> pathlib.Path:
    """Unpack into destination (replacing it); return the single top-level directory."""
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    root = destination.resolve()
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as bundle:
            for member in bundle.namelist():
                if not (root / member).resolve().is_relative_to(root):
                    raise Failure(f"{archive.name}: member escapes the extraction directory: {member}")
            bundle.extractall(root)
    else:
        with tarfile.open(archive) as bundle:
            bundle.extractall(root, filter="data")
    children = [child for child in destination.iterdir()]
    if len(children) == 1 and children[0].is_dir():
        return children[0]
    return destination


def run(command: list[str], cwd: pathlib.Path, env: dict[str, str]) -> None:
    print("+", " ".join(command), flush=True)
    result = subprocess.run(command, cwd=cwd, env=env)
    if result.returncode != 0:
        raise Failure(f"command failed ({result.returncode}): {' '.join(command)}")


def find_patch_tool() -> str:
    found = shutil.which("patch")
    if found:
        return found
    for candidate in (r"C:\Program Files\Git\usr\bin\patch.exe", r"C:\Program Files (x86)\Git\usr\bin\patch.exe"):
        if os.path.isfile(candidate):
            return candidate
    raise Failure("patch.exe not found (it ships with Git for Windows: <Git>\\usr\\bin\\patch.exe)")


class Builder:
    def __init__(self, pcsx2: pathlib.Path, prefix: pathlib.Path, work: pathlib.Path, force: bool) -> None:
        self.pcsx2 = pcsx2
        self.prefix = prefix
        self.work = work
        self.force = force
        self.table = sources()
        self.cache = work / "downloads"
        self.stamps = prefix / ".rrv-stamps"
        self.cache.mkdir(parents=True, exist_ok=True)
        self.stamps.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ)
        for name in ("CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS"):
            env.pop(name, None)  # the dependencies use the default MSVC compiler, as upstream's script does
        env["PATH"] = str(prefix / "bin") + os.pathsep + env["PATH"]
        self.env = env

    def cmake_args(self, *extra: str) -> list[str]:
        prefix = self.prefix.as_posix()
        # cl by name: with the Ninja generator CMake would otherwise find the clang.exe that Visual Studio puts on PATH.
        return ["cmake", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=cl",
                "-DCMAKE_CXX_COMPILER=cl", f"-DCMAKE_PREFIX_PATH={prefix}", f"-DCMAKE_INSTALL_PREFIX={prefix}",
                POLICY, *extra]

    def step(self, stamp: str, source: str | None, configure: list[str], *, subdir: str = ".",
             build_dir: str = "build", patches: list[tuple[str, pathlib.Path]] | None = None,
             extra_sources: dict[str, str] | None = None) -> None:
        marker = self.stamps / f"{stamp}.done"
        if marker.exists() and not self.force:
            print(f"skip {stamp} (built)")
            return
        print(f"== {stamp}")
        tree = extract(fetch(self.table[source], self.cache), self.work / stamp.replace("/", "-"))
        for name, relative in (extra_sources or {}).items():
            extra = extract(fetch(self.table[name], self.cache), self.work / (stamp + "-" + name))
            target = tree / relative
            if target.exists():
                shutil.rmtree(target)
            shutil.move(str(extra), str(target))
        for patch_name, patch_file in patches or []:
            # A checkout with core.eol=crlf (Git for Windows' default for text=auto files) holds the patch with CRLF
            # line ends; the extracted sources have LF.
            lf_patch = self.work / f"{patch_name}.lf.patch"
            lf_patch.write_bytes(patch_file.read_bytes().replace(b"\r\n", b"\n"))
            run([find_patch_tool(), "-p1", "-i", str(lf_patch)], tree, self.env)
        run([*self.cmake_args(*configure), "-S", subdir, "-B", build_dir], tree, self.env)
        run(["cmake", "--build", build_dir, "--parallel"], tree, self.env)
        run(["cmake", "--install", build_dir], tree, self.env)
        marker.write_text(str(self.table[source]["sha256"]) + "\n", encoding="utf-8")

    def build_all(self) -> None:
        shared = ["-DBUILD_SHARED_LIBS=ON"]
        self.step("zlib", "zlib", [*shared, "-DZLIB_BUILD_EXAMPLES=OFF"])
        self.step("libpng", "libpng", [*shared, "-DPNG_TESTS=OFF", "-DPNG_STATIC=OFF", "-DPNG_SHARED=ON",
                                       "-DPNG_TOOLS=OFF"])
        simd = [] if shutil.which("nasm") else ["-DWITH_SIMD=OFF"]  # the SIMD paths need NASM; the bridge does not
        self.step("libjpeg-turbo", "libjpeg-turbo", [*shared, "-DBUILD_STATIC_LIBS=OFF", *simd])
        self.step("lz4", "lz4", [*shared, "-DLZ4_BUILD_CLI=OFF", "-DLZ4_BUILD_LEGACY_LZ4C=OFF",
                                 "-DCMAKE_C_FLAGS=/wd4711 /wd5045"], subdir="build/cmake")
        freetype = ["-DFT_REQUIRE_ZLIB=TRUE", "-DFT_REQUIRE_PNG=TRUE", "-DFT_DISABLE_BZIP2=TRUE",
                    "-DFT_DISABLE_BROTLI=TRUE", *shared]
        self.step("freetype-nohb", "freetype", [*freetype, "-DFT_DISABLE_HARFBUZZ=TRUE"])
        self.step("harfbuzz", "harfbuzz", [*shared, "-DHB_BUILD_UTILS=OFF", "-DHB_BUILD_GPU=OFF"])
        self.step("freetype", "freetype", [*freetype, "-DFT_REQUIRE_HARFBUZZ=TRUE"])
        self.step("zstd", "zstd", [*shared, "-DZSTD_BUILD_SHARED=ON", "-DZSTD_BUILD_STATIC=OFF",
                                   "-DZSTD_BUILD_PROGRAMS=OFF"], subdir="build/cmake")
        self.step("libwebp", "libwebp", [*shared, "-DWEBP_BUILD_ANIM_UTILS=OFF", "-DWEBP_BUILD_CWEBP=OFF",
                                         "-DWEBP_BUILD_DWEBP=OFF", "-DWEBP_BUILD_GIF2WEBP=OFF",
                                         "-DWEBP_BUILD_IMG2WEBP=OFF", "-DWEBP_BUILD_VWEBP=OFF",
                                         "-DWEBP_BUILD_WEBPINFO=OFF", "-DWEBP_BUILD_WEBPMUX=OFF",
                                         "-DWEBP_BUILD_EXTRAS=OFF"])
        self.step("sdl3", "sdl3", [*shared, "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_VIDEO=OFF",
                                   "-DSDL_POWER=OFF", "-DSDL_SENSOR=OFF", "-DSDL_DIALOG=OFF", "-DSDL_TRAY=OFF",
                                   "-DSDL_TEST_LIBRARY=OFF"])
        self.step("plutovg", "plutovg", [*shared, "-DPLUTOVG_BUILD_EXAMPLES=OFF"])
        self.step("plutosvg", "plutosvg", [*shared, "-DPLUTOSVG_ENABLE_FREETYPE=ON", "-DPLUTOSVG_BUILD_EXAMPLES=OFF"])
        self.step("rapidyaml", "rapidyaml", shared)
        self.step("directx-headers", "directx-headers", ["-DDXHEADERS_BUILD_TEST=OFF",
                                                         "-DDXHEADERS_BUILD_GOOGLE_TEST=OFF"])
        shaderc_patch = self.pcsx2 / ".github" / "workflows" / "scripts" / "common" / "shaderc-changes.patch"
        self.step("shaderc", "shaderc", ["-DSHADERC_SKIP_TESTS=ON", "-DSHADERC_SKIP_EXAMPLES=ON",
                                         "-DSHADERC_SKIP_COPYRIGHT_CHECK=ON", "-DSHADERC_ENABLE_SHARED_CRT=ON"],
                  patches=[("shaderc-changes", shaderc_patch)],
                  extra_sources={"shaderc-glslang": "third_party/glslang",
                                 "shaderc-spirv-headers": "third_party/spirv-headers",
                                 "shaderc-spirv-tools": "third_party/spirv-tools"})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--pcsx2", type=pathlib.Path, default=DEFAULT_PCSX2, help="PCSX2 checkout (default: %(default)s)")
    parser.add_argument("--prefix", type=pathlib.Path, help="install prefix (default: <pcsx2>/deps)")
    parser.add_argument("--work", type=pathlib.Path, help="download and build directory (default: <pcsx2>/deps-build)")
    parser.add_argument("--force", action="store_true", help="rebuild steps that already have a stamp")
    parser.add_argument("--keep-work", action="store_true", help="keep the build directory afterwards")
    args = parser.parse_args()
    if sys.platform != "win32":
        raise Failure("this script builds the Windows dependencies; other platforms use their package manager")
    for tool in ("cl", "cmake", "ninja", "curl.exe"):
        if not shutil.which(tool):
            raise Failure(f"{tool} is not on PATH (run from a shell that has run vcvars64)")
    pcsx2 = args.pcsx2.resolve()
    if not (pcsx2 / "CMakeLists.txt").is_file():
        raise Failure(f"no PCSX2 checkout at {pcsx2} (python scripts/prepare_dependencies.py --prepare --component pcsx2)")
    prefix = (args.prefix or pcsx2 / "deps").resolve()
    work = (args.work or pcsx2 / "deps-build").resolve()
    builder = Builder(pcsx2, prefix, work, args.force)
    builder.build_all()
    if not args.keep_work:
        for child in work.iterdir():
            if child.name != "downloads":
                shutil.rmtree(child, ignore_errors=True)
    print(f"PCSX2 Windows dependencies installed in {prefix}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
