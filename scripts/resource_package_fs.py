"""Descriptor-relative primitives for ADR-0006's fixed package roots.

POSIX (macOS, Linux): a directory handle is an open `O_DIRECTORY | O_NOFOLLOW` descriptor, and every operation is
relative to it (`dir_fd=`), so no component of a path can be swapped for a link between the check and the use.

Windows has no directory descriptors to hang operations on. There a directory handle is a `WinDir`: the absolute path
of a directory that was checked, component by component, to be a plain directory (no symbolic link, junction or other
reparse point). Each operation joins one validated name onto it and re-checks the node it reaches (a file is
re-identified after it is opened). That is weaker than descriptor-relative access against an attacker who can already
write inside the build directory between two calls, and no weaker against everything the package contract checks for:
links, hard links, special nodes, and a tree that does not match its inventory.
"""
from __future__ import annotations
import contextlib, errno, hashlib, os, pathlib, stat, sys
from typing import Iterator, Union

_WINDOWS = sys.platform == "win32"
if _WINDOWS:
    import ctypes, ctypes.wintypes, msvcrt
else:
    import fcntl

class PackageFilesystemError(RuntimeError): pass

class WinDir:
    """A verified plain directory on Windows, standing where a directory descriptor stands on POSIX."""
    __slots__ = ("path",)
    def __init__(self, path: str) -> None: self.path = path

DirHandle = Union[int, WinDir]

_ASCII = frozenset(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-./")
_CLOEXEC = getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOINHERIT", 0)
_BINARY = getattr(os, "O_BINARY", 0)
_NOFOLLOW = getattr(os, "O_NOFOLLOW", 0)
_DIR = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | _NOFOLLOW | _CLOEXEC
_FILE = os.O_RDONLY | _NOFOLLOW | _CLOEXEC | _BINARY
_REPARSE_POINT = 0x400  # FILE_ATTRIBUTE_REPARSE_POINT
# Names the Windows file system reserves (a device, whatever the extension) or silently rewrites (a trailing dot or space).
_WINDOWS_DEVICES = frozenset(["con", "prn", "aux", "nul", *(f"com{n}" for n in range(1, 10)), *(f"lpt{n}" for n in range(1, 10))])
SWAP_LEFTOVER = "temporary.swap"

def checked_relative(value: object) -> str:
    if not isinstance(value, str) or not value or value.startswith("/") or value.endswith("/"):
        raise PackageFilesystemError("package path must be a nonempty relative file name")
    try: raw = value.encode("ascii")
    except UnicodeEncodeError as exc: raise PackageFilesystemError("package paths must be ASCII") from exc
    if any(c not in _ASCII for c in raw) or any(p in ("", ".", "..") for p in value.split("/")):
        raise PackageFilesystemError("package path is unsafe")
    if _WINDOWS and any(p.endswith(".") or p.split(".")[0].lower() in _WINDOWS_DEVICES for p in value.split("/")):
        raise PackageFilesystemError("package path names a Windows device or ends in a dot")
    return value

def collision_key(value: str) -> str: return value.lower()
def _is_reparse(info: os.stat_result) -> bool: return bool(getattr(info, "st_file_attributes", 0) & _REPARSE_POINT)
def is_regular(info: os.stat_result) -> bool: return stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and not _is_reparse(info)
def is_directory(info: os.stat_result) -> bool: return stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode) and not _is_reparse(info)

def sha256_fd(fd: int) -> str:
    os.lseek(fd, 0, os.SEEK_SET); h = hashlib.sha256()
    while chunk := os.read(fd, 1024 * 1024): h.update(chunk)
    os.lseek(fd, 0, os.SEEK_SET); return h.hexdigest()

# ---- directory handles -------------------------------------------------------------------------------------------

def _child(handle: WinDir, name: str) -> str:
    if name in ("", ".", "..") or any(c in name for c in "/\\:"):
        raise PackageFilesystemError("package name is not a single path component")
    return os.path.join(handle.path, name)

def close_dir(handle: DirHandle) -> None:
    if isinstance(handle, int): os.close(handle)

def dup_dir(handle: DirHandle) -> DirHandle:
    return os.dup(handle) if isinstance(handle, int) else WinDir(handle.path)

def listdir_dir(handle: DirHandle) -> list[str]:
    return os.listdir(handle if isinstance(handle, int) else handle.path)

def stat_at(handle: DirHandle, name: str) -> os.stat_result:
    """Stat a child without following a link."""
    if isinstance(handle, int): return os.stat(name, dir_fd=handle, follow_symlinks=False)
    return os.lstat(_child(handle, name))

def open_dir_at(handle: DirHandle, name: str) -> DirHandle:
    """Open a child directory; a link, reparse point or non-directory is refused."""
    if isinstance(handle, int): return os.open(name, _DIR, dir_fd=handle)
    path = _child(handle, name); info = os.lstat(path)
    if _is_reparse(info) or stat.S_ISLNK(info.st_mode): raise OSError(errno.ELOOP, "refusing to follow a link", path)
    if not stat.S_ISDIR(info.st_mode): raise NotADirectoryError(errno.ENOTDIR, "not a directory", path)
    return WinDir(path)

def open_file_at(handle: DirHandle, name: str) -> int:
    """Open a child for reading without following a link; returns a file descriptor."""
    if isinstance(handle, int): return os.open(name, _FILE, dir_fd=handle)
    path = _child(handle, name); before = os.lstat(path)
    if _is_reparse(before) or stat.S_ISLNK(before.st_mode): raise OSError(errno.ELOOP, "refusing to follow a link", path)
    fd = os.open(path, os.O_RDONLY | _CLOEXEC | _BINARY)
    try:
        after = os.fstat(fd)
        if before.st_ino and (after.st_dev, after.st_ino) != (before.st_dev, before.st_ino):
            raise OSError(errno.ELOOP, "file changed while it was opened", path)
    except BaseException: os.close(fd); raise
    return fd

def create_file_at(handle: DirHandle, name: str, *, read_write: bool = False) -> int:
    """Create a new file (mode 0600), failing if the name exists; returns a file descriptor."""
    flags = (os.O_RDWR if read_write else os.O_WRONLY) | os.O_CREAT | os.O_EXCL | _NOFOLLOW | _CLOEXEC | _BINARY
    if isinstance(handle, int): return os.open(name, flags, 0o600, dir_fd=handle)
    return os.open(_child(handle, name), flags, 0o600)

def mkdir_at(handle: DirHandle, name: str) -> None:
    if isinstance(handle, int): os.mkdir(name, 0o700, dir_fd=handle)
    else: os.mkdir(_child(handle, name))

def unlink_at(handle: DirHandle, name: str) -> None:
    if isinstance(handle, int): os.unlink(name, dir_fd=handle)
    else: os.unlink(_child(handle, name))

def rmdir_at(handle: DirHandle, name: str) -> None:
    if isinstance(handle, int): os.rmdir(name, dir_fd=handle)
    else: os.rmdir(_child(handle, name))

def rename_at(handle: DirHandle, source: str, destination: str) -> None:
    """Rename within one directory; an existing destination file is replaced atomically."""
    if isinstance(handle, int):
        os.rename(source, destination, src_dir_fd=handle, dst_dir_fd=handle); return
    # Windows: os.rename refuses an existing destination; os.replace swaps a file in atomically (MOVEFILE_REPLACE_EXISTING).
    source, destination = _child(handle, source), _child(handle, destination)
    if os.path.lexists(destination): os.replace(source, destination)
    else: os.rename(source, destination)

def swap_dirs_at(handle: DirHandle, first: str, second: str) -> None:
    """Exchange two directory entries of one directory.

    POSIX: one atomic system call (renameat2 RENAME_EXCHANGE on Linux, renameatx_np RENAME_SWAP on macOS). Windows has no
    exchange: the entries are rotated through SWAP_LEFTOVER with three renames, which a crash can interrupt after the
    first or second one. ensure_configured() clears that leftover and rebuilds, and every consumer verifies the tree
    against its inventory, so an interrupted swap is repaired and never trusted.
    """
    if not isinstance(handle, int):
        rename_at(handle, second, SWAP_LEFTOVER); rename_at(handle, first, second); rename_at(handle, SWAP_LEFTOVER, first); return
    import ctypes
    if sys.platform.startswith("linux"):
        # Linux: renameat2(RENAME_EXCHANGE), the equivalent atomic swap.
        libc = ctypes.CDLL(None, use_errno=True); call = libc.renameat2
        call.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]; call.restype = ctypes.c_int
        if call(handle, first.encode(), handle, second.encode(), 0x2) != 0: raise PackageFilesystemError("renameat2(RENAME_EXCHANGE) failed: " + os.strerror(ctypes.get_errno()))
        return
    libc = ctypes.CDLL("/usr/lib/libSystem.B.dylib", use_errno=True); call = libc.renameatx_np
    call.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]; call.restype = ctypes.c_int
    if call(handle, first.encode(), handle, second.encode(), 0x00000002) != 0: raise PackageFilesystemError("renameatx_np(RENAME_SWAP) failed: " + os.strerror(ctypes.get_errno()))

# ---- roots -------------------------------------------------------------------------------------------------------

def open_absolute_directory(path: pathlib.Path) -> DirHandle:
    if not path.is_absolute(): raise PackageFilesystemError("authority path is not absolute")
    if _WINDOWS:
        parts = path.parts; current = parts[0]
        for part in parts[1:]:
            current = os.path.join(current, part); info = os.lstat(current)
            if _is_reparse(info) or stat.S_ISLNK(info.st_mode):
                raise PackageFilesystemError(f"authority path contains symlink component: {part}")
            if not stat.S_ISDIR(info.st_mode): raise NotADirectoryError(errno.ENOTDIR, "not a directory", current)
        return WinDir(current)
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
        fd = open_file_at(parent, path.name)
        return fd
    finally: close_dir(parent)

def source_identity(path: pathlib.Path) -> tuple[int, int, int, str]:
    fd = open_absolute_file(path)
    try:
        info = os.fstat(fd)
        if not is_regular(info): raise PackageFilesystemError("pinned input is not a single-link regular file")
        return info.st_dev, info.st_ino, info.st_size, sha256_fd(fd)
    finally: os.close(fd)

def open_dir(path: pathlib.Path) -> DirHandle:
    handle = open_absolute_directory(path)
    try:
        info = os.fstat(handle) if isinstance(handle, int) else os.lstat(handle.path)
        if not is_directory(info): raise PackageFilesystemError("authority root is unsafe")
        return handle
    except Exception: close_dir(handle); raise

def open_beneath(root: DirHandle, relative: str, *, directory: bool = False) -> DirHandle | int:
    checked_relative(relative); handle = dup_dir(root)
    try:
        parts = relative.split("/")
        for index, part in enumerate(parts):
            child = open_dir_at(handle, part) if directory or index < len(parts) - 1 else open_file_at(handle, part)
            close_dir(handle); handle = child
        return handle
    except Exception: close_dir(handle); raise

def typed_inventory(path: pathlib.Path) -> list[dict[str, object]]:
    """Enumerate a package from its no-follow root handle."""
    root = open_absolute_directory(path.absolute())
    entries: list[dict[str, object]] = []
    try:
        def visit(handle: DirHandle, prefix: str) -> None:
            for name in sorted(listdir_dir(handle)):
                relative = f"{prefix}/{name}" if prefix else name
                info = stat_at(handle, name)
                if stat.S_ISLNK(info.st_mode) or _is_reparse(info):
                    raise PackageFilesystemError(f"resource package contains symlink: {relative}")
                if is_directory(info):
                    entries.append({"path": relative, "type": "directory"})
                    child = open_dir_at(handle, name)
                    try: visit(child, relative)
                    finally: close_dir(child)
                elif stat.S_ISREG(info.st_mode):
                    if info.st_nlink != 1:
                        raise PackageFilesystemError(f"resource package contains hard-linked file: {relative}")
                    child = open_file_at(handle, name)
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
        close_dir(root)

def mkdirs_beneath(root: DirHandle, relative: str) -> DirHandle:
    checked_relative(relative); handle = dup_dir(root)
    try:
        for part in relative.split("/"):
            try: mkdir_at(handle, part)
            except FileExistsError: pass
            child = open_dir_at(handle, part); close_dir(handle); handle = child
        return handle
    except Exception: close_dir(handle); raise

def remove_tree_beneath(root: DirHandle, name: str) -> None:
    checked_relative(name)
    if "/" in name: raise PackageFilesystemError("cleanup accepts only the fixed temporary child")
    info = stat_at(root, name)
    if not is_directory(info): raise PackageFilesystemError("temporary tree is unsafe")
    handle = open_dir_at(root, name)
    try:
        for entry in listdir_dir(handle):
            node = stat_at(handle, entry)
            if is_directory(node): remove_tree_beneath(handle, entry)
            elif is_regular(node): unlink_at(handle, entry)
            else: raise PackageFilesystemError("temporary tree contains an unsafe node")
    finally: close_dir(handle)
    rmdir_at(root, name)

def write_all(fd: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        count = os.write(fd, view)
        if count <= 0: raise PackageFilesystemError("short package write")
        view = view[count:]

# ---- the stable lock ---------------------------------------------------------------------------------------------

if _WINDOWS:
    class _Overlapped(ctypes.Structure):
        _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t), ("Offset", ctypes.wintypes.DWORD),
                    ("OffsetHigh", ctypes.wintypes.DWORD), ("hEvent", ctypes.wintypes.HANDLE)]

    def _lock_region(fd: int, exclusive: bool) -> None:
        """LockFileEx on the first byte: waits for other holders, shared for readers and exclusive for the producer."""
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.LockFileEx.argtypes = [ctypes.wintypes.HANDLE, ctypes.wintypes.DWORD, ctypes.wintypes.DWORD,
                                        ctypes.wintypes.DWORD, ctypes.wintypes.DWORD, ctypes.POINTER(_Overlapped)]
        kernel32.LockFileEx.restype = ctypes.wintypes.BOOL
        overlapped = _Overlapped()
        if not kernel32.LockFileEx(msvcrt.get_osfhandle(fd), 0x2 if exclusive else 0, 0, 1, 0, ctypes.byref(overlapped)):
            raise PackageFilesystemError("cannot lock the stable package lock: " + str(ctypes.WinError(ctypes.get_last_error())))

@contextlib.contextmanager
def lease(lock: pathlib.Path, exclusive: bool) -> Iterator[None]:
    fd = open_absolute_file(lock.absolute())
    try:
        if not is_regular(os.fstat(fd)): raise PackageFilesystemError("stable package lock is unsafe")
        if _WINDOWS: _lock_region(fd, exclusive)
        else: fcntl.flock(fd, fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH)
        yield
    finally: os.close(fd)
