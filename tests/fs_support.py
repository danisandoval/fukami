"""File-system helpers for tests that need links.

Windows grants file symbolic links only to Developer Mode or elevated users, but a directory junction needs no
privilege and is a reparse point the producer and the reader must refuse just the same. Directory links therefore use a
junction on Windows and a symbolic link elsewhere.
"""
from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile


def make_dir_link(link: pathlib.Path, target: pathlib.Path) -> None:
    if os.name == "nt":
        subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], check=True, capture_output=True)
    else:
        link.symlink_to(target, target_is_directory=True)


def is_dir_link(path: pathlib.Path) -> bool:
    return path.is_symlink() or (os.name == "nt" and bool(os.lstat(path).st_file_attributes & 0x400))


def remove_dir_link(link: pathlib.Path) -> None:
    if os.name == "nt":
        os.rmdir(link)
    else:
        link.unlink()


def symlinks_available() -> bool:
    """False on Windows without Developer Mode or elevation."""
    with tempfile.TemporaryDirectory(prefix="rrv-symlink-probe-") as probe:
        try:
            (pathlib.Path(probe) / "link").symlink_to(pathlib.Path(probe))
        except OSError:
            return False
    return True
