"""Descriptor-relative primitives for ADR-0006's fixed package roots."""
from __future__ import annotations
import contextlib, fcntl, hashlib, os, pathlib, stat
from typing import Iterator

class PackageFilesystemError(RuntimeError): pass

_ASCII = frozenset(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-./")
_DIR = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
_FILE = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC

def checked_relative(value: object) -> str:
    if not isinstance(value, str) or not value or value.startswith("/") or value.endswith("/"):
        raise PackageFilesystemError("package path must be a nonempty relative file name")
    try: raw = value.encode("ascii")
    except UnicodeEncodeError as exc: raise PackageFilesystemError("package paths must be ASCII") from exc
    if any(c not in _ASCII for c in raw) or any(p in ("", ".", "..") for p in value.split("/")):
        raise PackageFilesystemError("package path is unsafe")
    return value

def collision_key(value: str) -> str: return value.lower()
def is_regular(info: os.stat_result) -> bool: return stat.S_ISREG(info.st_mode) and info.st_nlink == 1
def is_directory(info: os.stat_result) -> bool: return stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode)

def sha256_fd(fd: int) -> str:
    os.lseek(fd, 0, os.SEEK_SET); h = hashlib.sha256()
    while chunk := os.read(fd, 1024 * 1024): h.update(chunk)
    os.lseek(fd, 0, os.SEEK_SET); return h.hexdigest()

def open_absolute_directory(path: pathlib.Path) -> int:
    if not path.is_absolute(): raise PackageFilesystemError("authority path is not absolute")
    fd = os.open("/", _DIR)
    try:
        for index, part in enumerate(path.parts[1:]):
            # Darwin's /var is a system-owned compatibility link to /private/var.
            # Canonicalize only this root alias; every caller-controlled component
            # below it remains descriptor-relative and O_NOFOLLOW.
            if index == 0 and part == "var" and os.path.islink("/var"):
                os.close(fd); fd = os.open("/private/var", _DIR); continue
            info = os.stat(part, dir_fd=fd, follow_symlinks=False)
            if stat.S_ISLNK(info.st_mode):
                raise PackageFilesystemError(f"authority path contains symlink component: {part}")
            child = os.open(part, _DIR, dir_fd=fd); os.close(fd); fd = child
        return fd
    except Exception: os.close(fd); raise

def open_absolute_file(path: pathlib.Path) -> int:
    if not path.is_absolute(): raise PackageFilesystemError("input path is not absolute")
    parent = open_absolute_directory(path.parent)
    try:
        fd = os.open(path.name, _FILE, dir_fd=parent)
        return fd
    finally: os.close(parent)

def source_identity(path: pathlib.Path) -> tuple[int, int, int, str]:
    fd = open_absolute_file(path)
    try:
        info = os.fstat(fd)
        if not is_regular(info): raise PackageFilesystemError("pinned input is not a single-link regular file")
        return info.st_dev, info.st_ino, info.st_size, sha256_fd(fd)
    finally: os.close(fd)

def open_dir(path: pathlib.Path) -> int:
    fd = open_absolute_directory(path)
    try:
        if not is_directory(os.fstat(fd)): raise PackageFilesystemError("authority root is unsafe")
        return fd
    except Exception: os.close(fd); raise

def open_beneath(root: int, relative: str, *, directory: bool = False) -> int:
    checked_relative(relative); fd = os.dup(root)
    try:
        parts = relative.split("/")
        for index, part in enumerate(parts):
            child = os.open(part, _DIR if directory or index < len(parts) - 1 else _FILE, dir_fd=fd)
            os.close(fd); fd = child
        return fd
    except Exception: os.close(fd); raise

def typed_inventory(path: pathlib.Path) -> list[dict[str, object]]:
    """Enumerate a package from its no-follow root descriptor."""
    root = open_absolute_directory(path.absolute())
    entries: list[dict[str, object]] = []
    try:
        def visit(fd: int, prefix: str) -> None:
            for name in sorted(os.listdir(fd)):
                relative = f"{prefix}/{name}" if prefix else name
                info = os.stat(name, dir_fd=fd, follow_symlinks=False)
                if stat.S_ISLNK(info.st_mode):
                    raise PackageFilesystemError(f"resource package contains symlink: {relative}")
                if is_directory(info):
                    entries.append({"path": relative, "type": "directory"})
                    child = os.open(name, _DIR, dir_fd=fd)
                    try: visit(child, relative)
                    finally: os.close(child)
                elif stat.S_ISREG(info.st_mode):
                    if info.st_nlink != 1:
                        raise PackageFilesystemError(f"resource package contains hard-linked file: {relative}")
                    child = os.open(name, _FILE, dir_fd=fd)
                    try:
                        checked = os.fstat(child)
                        if not is_regular(checked):
                            raise PackageFilesystemError(f"resource package file changed: {relative}")
                        entries.append({"path": relative, "type": "file", "size": checked.st_size,
                                        "sha256": sha256_fd(child)})
                    finally: os.close(child)
                else:
                    raise PackageFilesystemError(f"resource package contains unsafe node: {relative}")
        visit(root, "")
        return entries
    finally:
        os.close(root)

def mkdirs_beneath(root: int, relative: str) -> int:
    checked_relative(relative); fd = os.dup(root)
    try:
        for part in relative.split("/"):
            try: os.mkdir(part, 0o700, dir_fd=fd)
            except FileExistsError: pass
            child = os.open(part, _DIR, dir_fd=fd); os.close(fd); fd = child
        return fd
    except Exception: os.close(fd); raise

def remove_tree_beneath(root: int, name: str) -> None:
    checked_relative(name)
    if "/" in name: raise PackageFilesystemError("cleanup accepts only the fixed temporary child")
    info = os.stat(name, dir_fd=root, follow_symlinks=False)
    if not is_directory(info): raise PackageFilesystemError("temporary tree is unsafe")
    fd = os.open(name, _DIR, dir_fd=root)
    try:
        for entry in os.listdir(fd):
            node = os.stat(entry, dir_fd=fd, follow_symlinks=False)
            if is_directory(node): remove_tree_beneath(fd, entry)
            elif is_regular(node): os.unlink(entry, dir_fd=fd)
            else: raise PackageFilesystemError("temporary tree contains an unsafe node")
    finally: os.close(fd)
    os.rmdir(name, dir_fd=root)

def write_all(fd: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        count = os.write(fd, view)
        if count <= 0: raise PackageFilesystemError("short package write")
        view = view[count:]

@contextlib.contextmanager
def lease(lock: pathlib.Path, exclusive: bool) -> Iterator[None]:
    fd = open_absolute_file(lock.absolute())
    try:
        if not is_regular(os.fstat(fd)): raise PackageFilesystemError("stable package lock is unsafe")
        fcntl.flock(fd, fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH); yield
    finally: os.close(fd)
