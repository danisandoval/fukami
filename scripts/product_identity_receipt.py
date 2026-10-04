#!/usr/bin/env python3
"""Create and verify the fail-closed, source-bound RRV product identity receipt.

The receipt is deliberately a build-local artifact.  It records paths only as
locators; every locator used as evidence is accompanied by a size and SHA-256
digest.  User-supplied ELF and runtime data are intentionally not inputs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import platform
import re
import subprocess
import sys
from typing import Any


SCHEMA = 5
ROOT = Path(__file__).resolve().parents[1]


def fail(message: str) -> None:
    raise RuntimeError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_entry(path: Path, *, root: Path | None = None) -> dict[str, Any]:
    if not path.is_file():
        fail(f"required file is unavailable: {path}")
    return {"path": str(path if root is None else path.relative_to(root)),
            "size": path.stat().st_size, "sha256": sha256(path)}


def resolve(entry: dict[str, Any], *, base: Path | None = None) -> Path:
    value = entry.get("path")
    if not isinstance(value, str) or not value:
        fail("receipt entry omits a path")
    path = Path(value)
    return path if path.is_absolute() or base is None else base / path


def verify_file(entry: dict[str, Any], label: str, *, base: Path | None = None) -> None:
    path = resolve(entry, base=base)
    if not path.is_file():
        fail(f"{label} is unavailable: {path}")
    if path.stat().st_size != entry.get("size") or sha256(path) != entry.get("sha256"):
        fail(f"{label} hash/size mismatch: {path}")


def canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def run(*args: str, cwd: Path | None = None) -> str:
    completed = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    if completed.returncode:
        fail(f"command failed ({' '.join(args)}): {completed.stderr.strip()}")
    return completed.stdout


def git_state(source: Path) -> dict[str, Any]:
    if not (source / ".git").exists():
        fail(f"RRV source is not a Git checkout: {source}")
    status = run("git", "status", "--porcelain=v1", cwd=source)
    staged = run("git", "diff", "--cached", "--no-ext-diff", "--binary", cwd=source)
    unstaged = run("git", "diff", "--no-ext-diff", "--binary", cwd=source)
    raw_untracked = subprocess.run(["git", "ls-files", "--others", "--exclude-standard", "-z"],
                                   cwd=source, check=True, stdout=subprocess.PIPE).stdout
    untracked = []
    for raw in raw_untracked.split(b"\0"):
        if raw:
            relative = raw.decode("utf-8", "surrogateescape")
            untracked.append(path_state(source / relative, source))
    return {
        "path": str(source),
        "commit": run("git", "rev-parse", "HEAD", cwd=source).strip(),
        "status": status,
        "status_sha256": hashlib.sha256(status.encode()).hexdigest(),
        "staged_diff_sha256": hashlib.sha256(staged.encode()).hexdigest(),
        "unstaged_diff_sha256": hashlib.sha256(unstaged.encode()).hexdigest(),
        "clean": not status,
        "untracked": untracked,
        "untracked_sha256": hashlib.sha256(canonical(untracked)).hexdigest(),
    }


def path_state(path: Path, root: Path) -> dict[str, Any]:
    relative = str(path.relative_to(root))
    stat = path.lstat()
    if path.is_symlink():
        target = os.readlink(path)
        return {"path": relative, "kind": "symlink", "target": target,
                "sha256": hashlib.sha256(target.encode("utf-8", "surrogateescape")).hexdigest()}
    if not path.is_file():
        fail(f"untracked path is not a regular file or symlink: {path}")
    return {"path": relative, "kind": "file", "size": stat.st_size, "sha256": sha256(path)}


def git_source(path: Path, label: str) -> dict[str, Any]:
    state = git_state(path)
    state["label"] = label
    return state


def same_git_state(expected: dict[str, Any], current: dict[str, Any]) -> bool:
    """Compare every receipt-bound Git field, including NUL-safe untracked state."""
    fields = ("commit", "status", "status_sha256", "staged_diff_sha256",
              "unstaged_diff_sha256", "clean", "untracked", "untracked_sha256")
    return all(expected.get(field) == current.get(field) for field in fields)


def inventory(directory: Path) -> list[dict[str, Any]]:
    if not directory.is_dir():
        fail(f"required directory is unavailable: {directory}")
    return [file_entry(path, root=directory) for path in sorted(directory.rglob("*"))
            if path.is_file()]


def package_inventory(directory: Path) -> list[dict[str, Any]]:
    """Describe the published package without following non-package nodes."""
    from resource_package_fs import PackageFilesystemError, typed_inventory
    try:
        return typed_inventory(directory.absolute())
    except PackageFilesystemError as exc:
        fail(str(exc))
    root = os.lstat(directory)
    if not stat.S_ISDIR(root.st_mode) or stat.S_ISLNK(root.st_mode):
        fail(f"published resource package is not a real directory: {directory}")
    entries: list[dict[str, Any]] = []

    def visit(path: Path, relative: str) -> None:
        for child in sorted(path.iterdir(), key=lambda item: item.name):
            child_relative = f"{relative}/{child.name}" if relative else child.name
            info = os.lstat(child)
            if stat.S_ISLNK(info.st_mode):
                fail(f"published resource package contains symlink: {child_relative}")
            if stat.S_ISDIR(info.st_mode):
                entries.append({"path": child_relative, "type": "directory"})
                visit(child, child_relative)
            elif stat.S_ISREG(info.st_mode):
                if info.st_nlink != 1:
                    fail(f"published resource package contains hard-linked file: {child_relative}")
                entries.append({"path": child_relative, "type": "file", "size": info.st_size,
                                "sha256": sha256(child)})
            else:
                fail(f"published resource package contains non-regular node: {child_relative}")

    visit(directory, "")
    return entries


def inventory_digest(entries: list[dict[str, Any]]) -> str:
    return hashlib.sha256(canonical(entries)).hexdigest()


def parse_abi(header: Path) -> dict[str, Any]:
    contents = header.read_text(encoding="utf-8")
    match = re.search(r"RRV_PCSX2_GS_BRIDGE_ABI_VERSION\s*=\s*(\d+)", contents)
    if not match:
        fail(f"bridge ABI version is absent from {header}")
    return {"version": int(match.group(1)), "header": file_entry(header)}


def require_dict(data: Any, label: str) -> dict[str, Any]:
    if not isinstance(data, dict):
        fail(f"receipt omits {label}")
    return data


def require_list(data: Any, label: str) -> list[Any]:
    if not isinstance(data, list) or not data:
        fail(f"receipt omits {label}")
    return data


def metadata_from_ninja(build: Path, binary: Path) -> dict[str, Any]:
    for required in (build / "CMakeCache.txt", build / "build.ninja",
                     build / "compile_commands.json"):
        if not required.is_file():
            fail(f"product build metadata is unavailable: {required}")
    commands = run("ninja", "-C", str(build), "-t", "commands", "rrv-product")
    compile_commands = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    source_entries: list[dict[str, Any]] = []
    for entry in compile_commands:
        source = Path(entry.get("file", ""))
        if source.is_file():
            source_entries.append(file_entry(source))
    if not source_entries:
        fail("compile_commands.json contains no readable source inputs")
    graph = run("ninja", "-C", str(build), "-t", "graph", "rrv-product")
    labels = [json.loads('"' + value + '"')
              for value in re.findall(r'\[label="((?:\\.|[^"\\])*)"', graph)]
    objects = sorted({label for label in labels if label.endswith('.o')})
    if not objects:
        fail("product graph contains no auditable object files")
    dependencies = run("ninja", "-C", str(build), "-t", "deps", *objects)
    header_entries: list[dict[str, Any]] = []
    for line in dependencies.splitlines():
        if not line.startswith("    "):
            continue
        header = Path(line.strip())
        if not header.is_absolute():
            header = build / header
        if header.is_file():
            header_entries.append(file_entry(header))
    header_entries = sorted({(entry["path"], entry["size"], entry["sha256"])
                             for entry in header_entries})
    headers = [{"path": path, "size": size, "sha256": digest}
               for path, size, digest in header_entries]
    if not headers:
        fail("Ninja dependency records contain no readable effective header inputs")
    # The complete target closure is carried in Ninja's command text and compile
    # database.  Header dependency records are queried by the existing no-raylib
    # audit; bind that generated receipt too rather than attempting a lossy parser.
    audit = build / "product-no-raylib.json"
    if not audit.is_file():
        fail(f"post-link product audit is unavailable: {audit}")
    return {
        "build_directory": str(build),
        "cmake_cache": file_entry(build / "CMakeCache.txt"),
        "ninja_file": file_entry(build / "build.ninja"),
        "compile_commands": file_entry(build / "compile_commands.json"),
        "target_commands_sha256": hashlib.sha256(commands.encode()).hexdigest(),
        "target_commands_count": len([line for line in commands.splitlines() if line]),
        "source_inputs": sorted(source_entries, key=lambda item: item["path"]),
        "source_inputs_sha256": inventory_digest(sorted(source_entries, key=lambda item: item["path"])),
        "header_inputs": headers,
        "header_inputs_sha256": inventory_digest(headers),
        "post_link_audit": file_entry(audit),
    }


def validate_metadata(metadata: dict[str, Any], binary: Path) -> None:
    for key in ("cmake_cache", "ninja_file", "compile_commands", "post_link_audit"):
        verify_file(require_dict(metadata.get(key), f"build metadata {key}"), key)
    if not isinstance(metadata.get("target_commands_sha256"), str) or not metadata["target_commands_sha256"]:
        fail("receipt omits target link/compile command identity")
    sources = require_list(metadata.get("source_inputs"), "effective compiler source inputs")
    for index, entry in enumerate(sources):
        verify_file(require_dict(entry, f"source input {index}"), f"source input {index}")
    if inventory_digest(sources) != metadata.get("source_inputs_sha256"):
        fail("effective compiler source input order/hash mismatch")
    headers = require_list(metadata.get("header_inputs"), "effective compiler header inputs")
    for index, entry in enumerate(headers):
        verify_file(require_dict(entry, f"header input {index}"), f"header input {index}")
    if inventory_digest(headers) != metadata.get("header_inputs_sha256"):
        fail("effective compiler header input order/hash mismatch")
    build_dir = Path(metadata.get("build_directory", ""))
    current = metadata_from_ninja(build_dir, binary)
    if (current.get("target_commands_sha256") != metadata.get("target_commands_sha256") or
            current.get("target_commands_count") != metadata.get("target_commands_count")):
        fail("target compile/link metadata mismatch: "
             f"expected={metadata.get('target_commands_sha256')} current={current.get('target_commands_sha256')}")


def overlay_entry(spec: str) -> dict[str, Any]:
    if "=" not in spec:
        fail("--overlay-stage must be NAME=active:PATH or NAME=inactive")
    name, raw_state = spec.split("=", 1)
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", name):
        fail("overlay name must contain lowercase letters, digits and hyphens")
    if raw_state == "inactive":
        return {"name": name, "active": False}
    if not raw_state.startswith("active:"):
        fail("overlay stage must be active:PATH or inactive")
    return {"name": name, "active": True,
            "manifest": file_entry(Path(raw_state.removeprefix("active:")).resolve())}


def required_overlay_names(iop_mode: bool | None, rpc_mode: bool | None) -> list[str]:
    names = ["product-host", "m2-causal", "spr-pending-chain", "m2p-pad-observer",
             "pad-pressure", "m2p-game001-fail-closed", "product-host-final",
             "callback-stack-main-reservation"]
    if iop_mode is not None:
        names.append("iop-heap")
    if rpc_mode is not None:
        names.append("rpc-memory-safety")
    names.append("gs-control")
    return names


def check_overlay_order(overlays: list[Any], required: list[str]) -> None:
    names = [require_dict(item, "overlay").get("name") for item in overlays]
    if names != required:
        fail("ordered product overlay manifest sequence is missing or reordered")
    if len(set(names)) != len(names):
        fail("overlay manifest sequence contains duplicate names")
    for item in overlays:
        active = item.get("active")
        if not isinstance(active, bool):
            fail("overlay stage omits active state")
        if active:
            manifest = require_dict(item, "overlay").get("manifest")
            verify_file(require_dict(manifest, "overlay manifest"), "overlay manifest")
        elif "manifest" in item:
            fail("inactive overlay stage carries a manifest")


def overlay_script_entry(spec: str) -> dict[str, Any]:
    if "=" not in spec:
        fail("--overlay-script must be NAME=PATH")
    name, value = spec.split("=", 1)
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", name):
        fail("overlay script name is malformed")
    return {"name": name, "script": file_entry(Path(value).resolve())}


def check_overlay_scripts(scripts: list[Any], required: list[str]) -> None:
    names = [item.get("name") for item in scripts]
    if names != required:
        fail("overlay script stage set/order is missing or reordered")
    for item in scripts:
        verify_file(require_dict(item.get("script"), "overlay stage script"), "overlay stage script")


def check_overlay_pairing(overlays: list[Any], scripts: list[Any]) -> None:
    if [item.get("name") for item in overlays] != [item.get("name") for item in scripts]:
        fail("overlay stages and scripts must have the same ordered names")
    active = {item["name"]: item["active"] for item in overlays}
    if active.get("iop-heap") and not active.get("callback-stack-main-reservation"):
        fail("active IOP heap overlay requires active callback-stack reservation")
    if active.get("rpc-memory-safety") and not active.get("iop-heap"):
        fail("active RPC memory-safety overlay requires active IOP heap isolation")


def overlay_cache_mode(cache: str, option: str) -> bool | None:
    entries = [line for line in cache.splitlines() if line.startswith(option + ":")]
    if not entries:
        return None
    if len(entries) != 1 or entries[0] not in (option + ":BOOL=ON", option + ":BOOL=OFF"):
        fail(f"CMake cache has malformed {option} option")
    return entries[0].endswith("=ON")


def overlay_cache_modes(cache_entry: dict[str, Any]) -> tuple[bool | None, bool | None]:
    verify_file(cache_entry, "CMake cache")
    cache = resolve(cache_entry).read_text(encoding="utf-8")
    iop = overlay_cache_mode(cache, "RRV_IOP_HEAP_ISOLATION")
    rpc = overlay_cache_mode(cache, "RRV_RPC_MEMORY_SAFETY")
    if rpc is not None and iop is None:
        fail("RPC memory-safety cache option requires IOP heap cache option")
    return iop, rpc


def check_overlay_cache_binding(overlays: list[Any], modes: tuple[bool | None, bool | None],
                                *, creating: bool) -> None:
    if creating and any(mode is None for mode in modes):
        fail("new product receipt requires IOP and RPC options in CMake cache")
    for name, mode in zip(("iop-heap", "rpc-memory-safety"), modes):
        stage = next((item for item in overlays if item["name"] == name), None)
        if mode is None:
            if stage is not None:
                fail(f"legacy CMake cache forbids {name} overlay stage")
        elif stage is None or stage["active"] != mode:
            fail(f"{name} overlay state does not match CMake cache")


def create(args: argparse.Namespace) -> int:
    source = args.source_root.resolve()
    binary = args.binary.resolve()
    build = args.build_dir.resolve()
    generation = args.generation_manifest.resolve()
    bridge = args.bridge_manifest.resolve()
    bridge_verifier = args.bridge_manifest_verifier.resolve()
    bridge_verifier_text = args.bridge_manifest_verifier_text
    if not isinstance(bridge_verifier_text, str) or not bridge_verifier_text:
        fail("bridge manifest verifier configured path is unavailable")
    package_spec = args.resource_package_spec.resolve()
    package_root = args.resource_package_root.resolve()
    sdl_manifest = args.sdl_manifest.resolve()
    architecture = args.architecture
    abi_header = args.abi_header.resolve()
    if not binary.is_file():
        fail(f"product executable is unavailable: {binary}")
    for required in (generation, bridge, sdl_manifest, abi_header, package_spec):
        if not required.is_file():
            fail(f"required identity input is unavailable: {required}")
    generation_data = json.loads(generation.read_text(encoding="utf-8"))
    generated_dir = args.generated_dir.resolve()
    generated = require_dict(generation_data.get("generated"), "generation generated inventory")
    if not generated.get("files"):
        fail("generation manifest omits generated file inventory")
    bridge_data = json.loads(bridge.read_text(encoding="utf-8"))
    library_path = Path(require_dict(bridge_data.get("library"), "bridge library").get("path", ""))
    resources_path = Path(require_dict(bridge_data.get("runtime_resources"), "bridge resources").get("directory", ""))
    if not library_path.is_absolute():
        library_path = source / library_path
    if not resources_path.is_absolute():
        resources_path = source / resources_path
    sdl_data = json.loads(sdl_manifest.read_text(encoding="utf-8"))
    library_name = sdl_data.get("library")
    if not isinstance(library_name, str):
        fail("SDL install manifest omits library")
    sdl_library = sdl_manifest.parent / library_name
    package_data = json.loads(package_spec.read_text(encoding="utf-8"))
    if package_data.get("schema") != "rrv-resource-package-spec-v1":
        fail("unsupported resource package specification")
    if not package_root.is_dir():
        fail(f"published resource package is unavailable: {package_root}")
    if architecture in ("arm64", "aarch64", "ARM64"):
        # There is no VU JIT any more (2026-09-30): VU microcode is statically
        # recompiled. The ARM64 product still pins sse2neon.
        sse2neon = args.jit_source.resolve() if args.jit_source else None
        if sse2neon is None:
            fail("ARM64 product identity requires the SSE2NEON source input")
        jit_identity: dict[str, Any] = {"active": False, "architecture": architecture,
            "sse2neon": git_source(sse2neon, "pinned-sse2neon"),
            "selection": "no VU JIT: statically recompiled VU microcode"}
    else:
        jit_identity = {"active": False, "architecture": architecture,
                        "selection": "inactive on non-ARM64 effective architecture"}
    receipt = {
        "schema_version": SCHEMA,
        "kind": "rrv-product-identity",
        "source": git_state(source),
        "platform": {"system": platform.system(), "machine": platform.machine()},
        "product": {"binary": file_entry(binary)},
        "abi": parse_abi(abi_header),
        "generation": {"manifest": file_entry(generation), "output_directory": str(generated_dir),
                       "inventory": inventory(generated_dir),
                       "inventory_sha256": inventory_digest(inventory(generated_dir))},
        "overlays": [overlay_entry(spec) for spec in args.overlay_stage],
        "overlay_scripts": [overlay_script_entry(value) for value in args.overlay_script],
        "jit": jit_identity,
        "bridge": {"manifest": file_entry(bridge),
                   "verifier": {"configured_path": bridge_verifier_text,
                                "resolved": file_entry(bridge_verifier)},
                   "library": file_entry(library_path),
                   "runtime_resources": {"directory": str(resources_path), "files": inventory(resources_path),
                                         "sha256": inventory_digest(inventory(resources_path))},
                   "resource_package": {"spec": file_entry(package_spec),
                                        "version": package_data.get("version"),
                                        "composition": package_data.get("composition"),
                                        "bridge": package_data.get("bridge"),
                                        "inventory": package_data.get("inventory"),
                                        "provenance": package_data.get("provenance"),
                                        "published_root": str(package_root),
                                        "published_inventory": package_inventory(package_root),
                                        "published_inventory_sha256": inventory_digest(package_inventory(package_root))}},
        "metal": {"source_manifest": file_entry((lambda path: path if path.is_absolute() else source / path)(Path(require_dict(bridge_data.get("metal_resources"), "metal resources").get("source_manifest", "")))),
                  "verifier_manifest": file_entry((lambda path: path if path.is_absolute() else source / path)(Path(require_dict(bridge_data.get("metal_resources"), "metal resources").get("verifier_manifest", ""))))},
        "sdl": {"manifest": file_entry(sdl_manifest), "library": file_entry(sdl_library)},
        "build": metadata_from_ninja(build, binary),
        "package_linkage": "created post-link; runtime-manifest.json must bind this receipt before launch",
    }
    modes = overlay_cache_modes(require_dict(receipt["build"]["cmake_cache"], "CMake cache"))
    required = required_overlay_names(*modes)
    check_overlay_order(receipt["overlays"], required)
    check_overlay_scripts(receipt["overlay_scripts"], required)
    check_overlay_pairing(receipt["overlays"], receipt["overlay_scripts"])
    check_overlay_cache_binding(receipt["overlays"], modes, creating=True)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    print(f"created product identity receipt: {output} sha256={sha256(output)}")
    return 0


def verify(args: argparse.Namespace) -> int:
    receipt_path = args.receipt.resolve()
    if not receipt_path.is_file():
        fail(f"product identity receipt is unavailable: {receipt_path}")
    data = json.loads(receipt_path.read_text(encoding="utf-8"))
    if data.get("schema_version") != SCHEMA or data.get("kind") != "rrv-product-identity":
        fail("unsupported product identity receipt schema")
    source = require_dict(data.get("source"), "source identity")
    current = git_state(resolve(source))
    if not same_git_state(source, current):
        fail("RRV source dirty-state mismatch (including untracked path/byte state)")
    verify_file(require_dict(require_dict(data.get("product"), "product").get("binary"), "product binary"), "product executable")
    abi = require_dict(data.get("abi"), "ABI")
    verify_file(require_dict(abi.get("header"), "ABI header"), "ABI header")
    if parse_abi(resolve(abi["header"])) .get("version") != abi.get("version"):
        fail("bridge ABI version mismatch")
    generation = require_dict(data.get("generation"), "generation")
    verify_file(require_dict(generation.get("manifest"), "generation manifest"), "generation manifest")
    generated_dir = Path(generation.get("output_directory", ""))
    entries = require_list(generation.get("inventory"), "generated inventory")
    if inventory(generated_dir) != entries or inventory_digest(entries) != generation.get("inventory_sha256"):
        fail("generated source inventory mismatch")
    overlays = require_list(data.get("overlays"), "ordered overlay manifests")
    scripts = require_list(data.get("overlay_scripts"), "overlay scripts")
    build_identity = require_dict(data.get("build"), "build metadata")
    modes = overlay_cache_modes(require_dict(build_identity.get("cmake_cache"), "CMake cache"))
    required = required_overlay_names(*modes)
    check_overlay_order(overlays, required)
    check_overlay_scripts(scripts, required)
    check_overlay_pairing(overlays, scripts)
    check_overlay_cache_binding(overlays, modes, creating=False)
    jit = require_dict(data.get("jit"), "JIT identity")
    if jit.get("active"):
        if jit.get("architecture") not in ("arm64", "aarch64", "ARM64"):
            fail("active JIT identity has non-ARM64 architecture")
        for name in ("sse2neon", "vixl"):
            jit_source = require_dict(jit.get(name), f"JIT {name} identity")
            jit_current = git_state(resolve(jit_source))
            if not same_git_state(jit_source, jit_current):
                fail(f"JIT {name} source dirty-state mismatch")
    elif jit.get("architecture") in ("arm64", "aarch64", "ARM64"):
        if "vixl" in jit:
            fail("inactive JIT identity is inconsistent")
        sse2neon = require_dict(jit.get("sse2neon"), "ARM64 sse2neon identity")
        if not same_git_state(sse2neon, git_state(resolve(sse2neon))):
            fail("sse2neon source dirty-state mismatch")
    elif "sse2neon" in jit or "vixl" in jit:
        fail("inactive JIT identity is inconsistent")
    bridge = require_dict(data.get("bridge"), "bridge")
    verify_file(require_dict(bridge.get("manifest"), "bridge manifest"), "bridge manifest")
    verifier = require_dict(bridge.get("verifier"), "bridge manifest verifier")
    if not isinstance(verifier.get("configured_path"), str) or not verifier["configured_path"]:
        fail("bridge manifest verifier configured path is missing")
    verify_file(require_dict(verifier.get("resolved"), "bridge manifest verifier resolved path"), "bridge manifest verifier")
    verify_file(require_dict(bridge.get("library"), "bridge library"), "bridge library")
    resources = require_dict(bridge.get("runtime_resources"), "bridge resources")
    actual_resources = inventory(Path(resources.get("directory", "")))
    if actual_resources != require_list(resources.get("files"), "bridge resource inventory") or inventory_digest(actual_resources) != resources.get("sha256"):
        fail("bridge runtime resource inventory mismatch")
    package = require_dict(bridge.get("resource_package"), "source-derived resource package")
    verify_file(require_dict(package.get("spec"), "resource package spec"), "resource package spec")
    spec = json.loads(resolve(package["spec"]).read_text(encoding="utf-8"))
    if (spec.get("schema") != "rrv-resource-package-spec-v1" or
            spec.get("version") != package.get("version") or
            spec.get("composition") != package.get("composition") or
            spec.get("bridge") != package.get("bridge") or
            spec.get("inventory") != package.get("inventory") or
            spec.get("provenance") != package.get("provenance")):
        fail("resource package specification identity mismatch")
    published_root = Path(package.get("published_root", ""))
    published = require_list(package.get("published_inventory"), "published package inventory")
    if (package_inventory(published_root) != published or
            inventory_digest(published) != package.get("published_inventory_sha256")):
        fail("published resource package inventory mismatch")
    metal = require_dict(data.get("metal"), "Metal identity")
    verify_file(require_dict(metal.get("source_manifest"), "Metal source manifest"), "Metal source manifest")
    verify_file(require_dict(metal.get("verifier_manifest"), "Metal verifier manifest"), "Metal verifier manifest")
    sdl = require_dict(data.get("sdl"), "SDL identity")
    verify_file(require_dict(sdl.get("manifest"), "SDL manifest"), "SDL manifest")
    verify_file(require_dict(sdl.get("library"), "SDL library"), "SDL library")
    validate_metadata(require_dict(data.get("build"), "build metadata"), resolve(require_dict(data["product"].get("binary"), "product binary")))
    print(f"verified product identity receipt: {receipt_path} sha256={sha256(receipt_path)}")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    create_parser = modes.add_parser("create")
    create_parser.add_argument("--source-root", type=Path, default=ROOT)
    create_parser.add_argument("--build-dir", type=Path, required=True)
    create_parser.add_argument("--binary", type=Path, required=True)
    create_parser.add_argument("--generation-manifest", type=Path, required=True)
    create_parser.add_argument("--generated-dir", type=Path, required=True)
    create_parser.add_argument("--bridge-manifest", type=Path, required=True)
    create_parser.add_argument("--bridge-manifest-verifier", type=Path, required=True)
    create_parser.add_argument("--bridge-manifest-verifier-text", required=True)
    create_parser.add_argument("--resource-package-spec", type=Path, required=True)
    create_parser.add_argument("--resource-package-root", type=Path, required=True)
    create_parser.add_argument("--sdl-manifest", type=Path, required=True)
    create_parser.add_argument("--architecture", required=True)
    # --jit-source is the pinned sse2neon checkout (historical name).
    create_parser.add_argument("--jit-source", type=Path)
    create_parser.add_argument("--abi-header", type=Path, required=True)
    create_parser.add_argument("--overlay-stage", action="append", required=True)
    create_parser.add_argument("--overlay-script", action="append", required=True)
    create_parser.add_argument("--output", type=Path, required=True)
    verify_parser = modes.add_parser("verify")
    verify_parser.add_argument("--receipt", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        return create(args) if args.mode == "create" else verify(args)
    except (RuntimeError, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"product identity verification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
