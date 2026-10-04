#!/usr/bin/env python3
"""ADR-0006 configuration and atomic producer for one fixed package."""
from __future__ import annotations
import argparse, ctypes, hashlib, json, os, pathlib, platform, stat, subprocess, sys
from typing import Any
from resource_package_fs import (PackageFilesystemError, checked_relative, collision_key, is_directory,
    is_regular, lease, mkdirs_beneath, open_absolute_file, open_beneath, open_dir, remove_tree_beneath,
    sha256_fd, source_identity, write_all)

sys.dont_write_bytecode = True
INPUT_SCHEMA = "rrv-resource-package-input-v1"; SPEC_SCHEMA = "rrv-resource-package-spec-v1"; VERSION = "rrv-resource-package-v1"
class PackageError(RuntimeError): pass
def canonical(v: Any) -> bytes: return json.dumps(v, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode("ascii")
def digest(v: bytes) -> str: return hashlib.sha256(v).hexdigest()

def fixed(build: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path, pathlib.Path, pathlib.Path]:
    root = build.resolve()
    return root, root / ".rrv-resource-package.lock", root / "CMakeFiles/rrv-resource-package/spec.json", root / ".rrv-resource-packages"

def native() -> None:
    if sys.platform.startswith("linux") and platform.machine() == "x86_64": return  # Gate 5 (Linux/Steam Deck)
    if sys.platform != "darwin" or platform.machine() != "arm64": raise PackageError("ADR-0006 requires native macOS arm64 or Linux x86-64")

def read_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        fd = open_absolute_file(path.resolve())
        try: data = b"".join(iter(lambda: os.read(fd, 1024 * 1024), b""))
        finally: os.close(fd)
        value = json.loads(data.decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc: raise PackageError("cannot read package configuration") from exc
    if not isinstance(value, dict): raise PackageError("package configuration is not an object")
    return value

def overlaps(a: pathlib.Path, b: pathlib.Path) -> bool:
    try: a.relative_to(b); return True
    except ValueError:
        try: b.relative_to(a); return True
        except ValueError: return False

def configured_spec(manifest: pathlib.Path, build: pathlib.Path, output: pathlib.Path) -> dict[str, Any]:
    raw = read_json(manifest)
    if raw.get("schema") != INPUT_SCHEMA or set(raw) - {"schema", "bridge", "entries", "directories", "provenance", "provenance_sources", "bridge_provenance"}:
        raise PackageError("unsupported package input schema")
    if not isinstance(raw.get("bridge"), str) or not isinstance(raw.get("entries"), list): raise PackageError("package input lacks bridge or entries")
    provenance = raw.get("provenance", {})
    if not isinstance(provenance, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in provenance.items()): raise PackageError("provenance must be a string map")
    root, lock, fixed_spec, artifacts = fixed(build)
    if output.parent.resolve() / output.name != fixed_spec: raise PackageError("specification path is not the fixed control path")
    names: set[str] = {"identity"}; collisions: set[str] = {"identity"}; inventory: list[dict[str, Any]] = []; sources: list[dict[str, Any]] = []; provenance_sources: list[dict[str, Any]] = []
    def pin_source(key: str, value: str) -> dict[str, Any]:
        source = pathlib.Path(value); source = source if source.is_absolute() else manifest.parent / source
        source = source.parent.resolve() / source.name
        if any(overlaps(source, protected) for protected in (root / "CMakeFiles/rrv-resource-package", artifacts, lock)): raise PackageError("input overlaps package control/output namespace")
        dev, ino, size, sha = source_identity(source)
        return {"key": key, "source": str(source), "device": dev, "inode": ino, "size": size, "sha256": sha}
    for entry in raw["entries"]:
        if not isinstance(entry, dict) or set(entry) != {"path", "source"}: raise PackageError("input entries require only path and source")
        path = checked_relative(entry["path"])
        if path in names or collision_key(path) in collisions: raise PackageError("payload paths collide")
        pinned = pin_source(path, entry["source"]); source = pathlib.Path(pinned["source"])
        dev, ino, size, sha = pinned["device"], pinned["inode"], pinned["size"], pinned["sha256"]
        inventory.append({"path": path, "type": "file", "size": size, "sha256": sha})
        sources.append({"path": path, "source": str(source), "device": dev, "inode": ino, "size": size, "sha256": sha})
        names.add(path); collisions.add(collision_key(path))
    raw_provenance_sources = raw.get("provenance_sources", {})
    if not isinstance(raw_provenance_sources, dict) or not all(isinstance(k, str) and isinstance(v, str) and k for k, v in raw_provenance_sources.items()): raise PackageError("provenance_sources must be a non-empty-string path map")
    for key, value in raw_provenance_sources.items(): provenance_sources.append(pin_source(key, value))
    bridge_provenance = raw.get("bridge_provenance")
    if bridge_provenance is not None:
        keys = {"manifest", "verifier", "expected_rrv_commit", "expected_pcsx2_commit", "expected_bridge_patch_sha256"}
        if not isinstance(bridge_provenance, dict) or set(bridge_provenance) != keys or not all(isinstance(bridge_provenance[key], str) and bridge_provenance[key] and "\0" not in bridge_provenance[key] for key in keys): raise PackageError("bridge_provenance is malformed")
        bridge_provenance = {
            **pin_source("bridge-manifest", bridge_provenance["manifest"]),
            "verifier": pin_source("bridge-manifest-verifier", bridge_provenance["verifier"]),
            **{key: bridge_provenance[key] for key in keys - {"manifest", "verifier"}},
        }
    if not inventory: raise PackageError("package inventory is empty")
    directories = raw.get("directories", [])
    if not isinstance(directories, list): raise PackageError("directories must be a list")
    for value in directories:
        path = checked_relative(value)
        if path in names or collision_key(path) in collisions: raise PackageError("directory paths collide")
        inventory.append({"path": path, "type": "directory"}); names.add(path); collisions.add(collision_key(path))
    for path in list(names):
        parts = path.split("/")[:-1]
        for end in range(1, len(parts) + 1):
            parent = "/".join(parts[:end])
            if parent not in names:
                if collision_key(parent) in collisions: raise PackageError("package prefix paths collide")
                inventory.append({"path": parent, "type": "directory"}); names.add(parent); collisions.add(collision_key(parent))
    files = {row["path"] for row in inventory if row["type"] == "file"}
    if any(any(other.startswith(file + "/") for other in names) for file in files): raise PackageError("file path is an inventory prefix")
    bridge = checked_relative(raw["bridge"])
    if bridge not in files: raise PackageError("bridge must be an inventory file")
    inventory.sort(key=lambda r: (r["path"], r["type"])); sources.sort(key=lambda r: r["path"]); provenance_sources.sort(key=lambda r: r["key"])
    composition_bridge = None if bridge_provenance is None else {
        **{key: value for key, value in bridge_provenance.items() if key not in {"inode", "verifier"}},
        "verifier": {key: value for key, value in bridge_provenance["verifier"].items() if key != "inode"},
    }
    composition = digest(canonical({"version": VERSION, "bridge": bridge, "inventory": inventory, "provenance": provenance,
                                    "sources": [{k: v for k, v in row.items() if k != "inode"} for row in sources], "provenance_sources": [{k: v for k, v in row.items() if k != "inode"} for row in provenance_sources], "bridge_provenance": composition_bridge}))
    return {"schema": SPEC_SCHEMA, "version": VERSION, "composition": composition, "bridge": bridge, "inventory": inventory, "sources": sources, "provenance": provenance, "provenance_sources": provenance_sources, "bridge_provenance": bridge_provenance}

def write_spec(spec: dict[str, Any], build: pathlib.Path, output: pathlib.Path) -> None:
    _, lock, fixed_spec, _ = fixed(build)
    if output.parent.resolve() / output.name != fixed_spec: raise PackageError("cannot publish spec outside fixed control path")
    build_fd = open_dir(build.resolve())
    try:
        control_fd = mkdirs_beneath(build_fd, "CMakeFiles/rrv-resource-package")
        try:
            try: lock_fd = os.open(".rrv-resource-package.lock", os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=build_fd)
            except FileExistsError: lock_fd = open_beneath(build_fd, ".rrv-resource-package.lock")
            try:
                if not is_regular(os.fstat(lock_fd)): raise PackageError("stable lock is unsafe")
            finally: os.close(lock_fd)
            try: os.unlink(".spec.tmp", dir_fd=control_fd)
            except FileNotFoundError: pass
            spec_fd = os.open(".spec.tmp", os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=control_fd)
            try: write_all(spec_fd, canonical(spec)); os.fsync(spec_fd)
            finally: os.close(spec_fd)
            os.rename(".spec.tmp", "spec.json", src_dir_fd=control_fd, dst_dir_fd=control_fd)
        finally: os.close(control_fd)
    finally: os.close(build_fd)

def load_spec(build: pathlib.Path, spec_path: pathlib.Path, expected: str) -> dict[str, Any]:
    _, _, fixed_spec, _ = fixed(build)
    if spec_path.parent.resolve() / spec_path.name != fixed_spec or len(expected) != 64: raise PackageError("producer command lacks fixed pinned spec")
    build_fd = open_dir(build.resolve())
    try:
        fd = open_beneath(build_fd, "CMakeFiles/rrv-resource-package/spec.json")
        try: data = b"".join(iter(lambda: os.read(fd, 1024 * 1024), b""))
        finally: os.close(fd)
    finally: os.close(build_fd)
    if digest(data) != expected: raise PackageError("fixed spec integrity mismatch")
    spec = json.loads(data.decode("ascii"))
    if not isinstance(spec, dict) or set(spec) != {"schema", "version", "composition", "bridge", "inventory", "sources", "provenance", "provenance_sources", "bridge_provenance"} or spec.get("schema") != SPEC_SCHEMA or spec.get("version") != VERSION:
        raise PackageError("fixed specification is malformed")
    return spec

def verify_pins(spec: dict[str, Any]) -> None:
    for source in [*spec["sources"], *spec["provenance_sources"]]:
        if source_identity(pathlib.Path(source["source"])) != (source["device"], source["inode"], source["size"], source["sha256"]):
            raise PackageError("pinned source provenance changed; reconfigure required")
    bridge = spec["bridge_provenance"]
    if bridge is not None:
        if source_identity(pathlib.Path(bridge["source"])) != (bridge["device"], bridge["inode"], bridge["size"], bridge["sha256"]): raise PackageError("pinned bridge manifest changed; reconfigure required")
        verifier = bridge["verifier"]
        if source_identity(pathlib.Path(verifier["source"])) != (verifier["device"], verifier["inode"], verifier["size"], verifier["sha256"]): raise PackageError("pinned strict bridge verifier changed; reconfigure required")
        result = subprocess.run([sys.executable, "-B", verifier["source"], "verify", "--manifest", bridge["source"], "--expected-rrv-commit", bridge["expected_rrv_commit"], "--expected-pcsx2-commit", bridge["expected_pcsx2_commit"], "--expected-bridge-patch-sha256", bridge["expected_bridge_patch_sha256"]], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if result.returncode: raise PackageError("strict provenance verification failed")

def identity(spec: dict[str, Any]) -> bytes: return f"{VERSION}\n{spec['composition']}\n".encode("ascii")

def verify_tree(artifact_fd: int, name: str, spec: dict[str, Any]) -> bool:
    try: root = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=artifact_fd)
    except FileNotFoundError: return False
    try:
        expected = {row["path"]: row for row in spec["inventory"]}; actual: dict[str, str] = {}
        def visit(fd: int, prefix: str) -> bool:
            for entry in os.listdir(fd):
                node = os.stat(entry, dir_fd=fd, follow_symlinks=False); path = entry if not prefix else prefix + "/" + entry
                if is_directory(node):
                    actual[path] = "directory"; child = os.open(entry, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=fd)
                    try:
                        if not visit(child, path): return False
                    finally: os.close(child)
                elif is_regular(node): actual[path] = "file"
                else: return False
            return True
        if not visit(root, "") or actual.pop("identity", None) != "file" or set(actual) != set(expected): return False
        fd = open_beneath(root, "identity")
        try: valid_identity = os.read(fd, 4096) == identity(spec)
        finally: os.close(fd)
        if not valid_identity: return False
        for path, row in expected.items():
            if actual[path] != row["type"]: return False
            if row["type"] == "file":
                fd = open_beneath(root, path)
                try:
                    node = os.fstat(fd)
                    if not is_regular(node) or node.st_size != row["size"] or sha256_fd(fd) != row["sha256"]: return False
                finally: os.close(fd)
        return True
    finally: os.close(root)

def preflight_removable_tree(artifact_fd: int, name: str) -> bool:
    """Return false for an absent package; reject any existing unsafe tree.

    A corrupt but ordinary directory tree is deliberately removable, so a later
    typed rebuild can repair it.  This check is about deletion authority only:
    no symlinks, special files, or multi-link regular files may ever cross the
    publication boundary into the producer's cleanup path.
    """
    try: root = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=artifact_fd)
    except FileNotFoundError: return False
    except OSError as exc: raise PackageError("existing current package root is unsafe") from exc
    try:
        def visit(fd: int) -> None:
            for entry in os.listdir(fd):
                node = os.stat(entry, dir_fd=fd, follow_symlinks=False)
                if is_directory(node):
                    child = os.open(entry, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=fd)
                    try: visit(child)
                    finally: os.close(child)
                elif not is_regular(node):
                    raise PackageError("existing current package tree is unsafe")
        visit(root)
        return True
    finally: os.close(root)

def build_temporary(artifact_fd: int, spec: dict[str, Any]) -> None:
    os.mkdir("temporary", 0o700, dir_fd=artifact_fd); root = os.open("temporary", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=artifact_fd)
    try:
        for row in spec["inventory"]:
            if row["type"] == "directory": fd = mkdirs_beneath(root, row["path"]); os.close(fd)
        for source in spec["sources"]:
            parent, _, leaf = source["path"].rpartition("/"); output = mkdirs_beneath(root, parent) if parent else os.dup(root)
            try:
                inp = open_absolute_file(pathlib.Path(source["source"]))
                try:
                    out = os.open(leaf, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600, dir_fd=output)
                    try:
                        while chunk := os.read(inp, 1024 * 1024): write_all(out, chunk)
                    finally: os.close(out)
                finally: os.close(inp)
            finally: os.close(output)
        fd = os.open("identity", os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600, dir_fd=root)
        try: write_all(fd, identity(spec))
        finally: os.close(fd)
    finally: os.close(root)

def atomic_swap(fd: int) -> None:
    if sys.platform.startswith("linux"):
        # Linux: renameat2(RENAME_EXCHANGE), the equivalent atomic swap.
        libc = ctypes.CDLL(None, use_errno=True); call = libc.renameat2
        call.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]; call.restype = ctypes.c_int
        if call(fd, b"temporary", fd, b"current", 0x2) != 0: raise PackageError("renameat2(RENAME_EXCHANGE) failed: " + os.strerror(ctypes.get_errno()))
        return
    libc = ctypes.CDLL("/usr/lib/libSystem.B.dylib", use_errno=True); call = libc.renameatx_np
    call.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]; call.restype = ctypes.c_int
    if call(fd, b"temporary", fd, b"current", 0x00000002) != 0: raise PackageError("renameatx_np(RENAME_SWAP) failed: " + os.strerror(ctypes.get_errno()))

def ensure_configured(build: pathlib.Path, expected: str) -> None:
    _, lock, spec_path, artifacts = fixed(build)
    with lease(lock, False):
        spec = load_spec(build, spec_path, expected); verify_pins(spec)
        build_fd = open_dir(build.resolve())
        try:
            try: fd = open_beneath(build_fd, ".rrv-resource-packages", directory=True)
            except FileNotFoundError: fd = -1
        finally: os.close(build_fd)
        if fd >= 0:
            try:
                try: os.stat("temporary", dir_fd=fd, follow_symlinks=False); temporary = True
                except FileNotFoundError: temporary = False
                if verify_tree(fd, "current", spec) and not temporary: return
            finally: os.close(fd)
    with lease(lock, True):
        spec = load_spec(build, spec_path, expected); verify_pins(spec)
        build_fd = open_dir(build.resolve())
        try: fd = mkdirs_beneath(build_fd, ".rrv-resource-packages")
        finally: os.close(build_fd)
        try:
            # Never clean temporary or construct a replacement until the old
            # public tree has been classified as safe to move into temporary.
            current_exists = preflight_removable_tree(fd, "current")
            try: os.stat("temporary", dir_fd=fd, follow_symlinks=False)
            except FileNotFoundError: temporary_exists = False
            else: temporary_exists = True
            if temporary_exists: remove_tree_beneath(fd, "temporary")
            if verify_tree(fd, "current", spec): return
            build_temporary(fd, spec)
            if not verify_tree(fd, "temporary", spec): raise PackageError("temporary package validation failed")
            if not current_exists: os.rename("temporary", "current", src_dir_fd=fd, dst_dir_fd=fd)
            else:
                preflight_removable_tree(fd, "current")
                atomic_swap(fd); remove_tree_beneath(fd, "temporary")
        finally: os.close(fd)

def main() -> int:
    parser = argparse.ArgumentParser(); commands = parser.add_subparsers(dest="command", required=True)
    config = commands.add_parser("configure"); config.add_argument("--input", type=pathlib.Path, required=True); config.add_argument("--build-root", type=pathlib.Path, required=True); config.add_argument("--spec", type=pathlib.Path, required=True)
    inspect = commands.add_parser("inspect"); inspect.add_argument("--input", type=pathlib.Path, required=True); inspect.add_argument("--build-root", type=pathlib.Path, required=True); inspect.add_argument("--spec", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        native()
        if args.command == "configure":
            spec = configured_spec(args.input, args.build_root, args.spec); write_spec(spec, args.build_root, args.spec); print(digest(canonical(spec)))
        else:
            spec = configured_spec(args.input, args.build_root, args.spec); print(spec["composition"])
    except (PackageError, PackageFilesystemError, OSError, ValueError, UnicodeError) as exc:
        print("resource-package: FAIL: " + str(exc), file=sys.stderr); return 1
    return 0
if __name__ == "__main__": raise SystemExit(main())
