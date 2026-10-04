#!/usr/bin/env python3
"""Client for the opt-in PCSX2 debug server (`tools/patches/pcsx2-debug-server.patch`).

The server speaks newline-delimited JSON over TCP, so no Node or MCP bridge is
needed to drive it. See `docs/instrumentation/PCSX2.md` section 5.1 for how to
build and launch the emulator with it.

This is the *interactive* counterpart to the aggregate `RRV_GT_*` probes: it
answers "what is the reference's state right here, right now", which PINE
(read-only, no execution control) cannot.

    # sanity check a running server
    python3 scripts/pcsx2_debug.py --port 21599 status

    # where is the guest, and what is it about to run
    python3 scripts/pcsx2_debug.py pause
    python3 scripts/pcsx2_debug.py disassemble 0x002dc840 --count 8

    # which guest PC writes this word? (interactive; see the warning below)
    python3 scripts/pcsx2_debug.py set_memcheck 0x334E94 --size 4 --type write
    python3 scripts/pcsx2_debug.py resume
    python3 scripts/pcsx2_debug.py get_backtrace

    # raw escape hatch
    python3 scripts/pcsx2_debug.py raw '{"cmd":"evaluate","expression":"sp+0x10"}'

WARNINGS
  * Read-only, or it is not ground truth. `write_memory` / `write_register` /
    `set_pc` are deliberately NOT wrapped here, for the same reason
    `scripts/pine.py` does not wrap `MsgWrite*`: a reference emulator that has
    been written to is no longer a reference.
  * `disassemble` needs a numeric address. `"pc"` returns an empty instruction
    list rather than an error -- read `status`'s `pc` first.
  * Server-side parameter names are not what you would guess: `read_memory`
    takes `length` (not `size`), and the memcheck commands take a half-open
    `[address, end)` range (not a size). This wrapper converts `--size` for you.
  * Do not arm `RRV_GT_WATCH` and a `set_memcheck` watchpoint on the same range
    in one session -- see `docs/instrumentation/PCSX2.md` section 5.1.
"""
from __future__ import annotations

import argparse
import json
import socket
import sys

DEFAULT_PORT = 21512


class DebugServer:
    """One persistent connection. Keep it open across many commands."""

    def __init__(self, host: str = "127.0.0.1", port: int = DEFAULT_PORT, timeout: float = 15.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self._buf = b""

    def cmd(self, name: str, **kwargs):
        kwargs["cmd"] = name
        self.sock.sendall((json.dumps(kwargs) + "\n").encode())
        while b"\n" not in self._buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("debug server closed the connection")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return json.loads(line)

    def close(self):
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def _int(text: str) -> int:
    return int(text, 0)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    sub = ap.add_subparsers(dest="action", required=True)

    for simple in ("status", "pause", "resume", "step", "step_over",
                   "get_threads", "get_modules", "get_backtrace",
                   "list_breakpoints", "list_memchecks", "clear_breakpoints"):
        sub.add_parser(simple)

    p = sub.add_parser("read_registers")
    p.add_argument("--cpu", default="ee", choices=["ee", "iop"])
    p.add_argument("--category", type=int, default=-1,
                   help="0=GPR 1=CP0 2=FPR 3=FCR 4=VU0F 5=VU0I 6=GSPRIV; -1=all")

    p = sub.add_parser("read_memory")
    p.add_argument("address", type=_int)
    p.add_argument("--length", type=int, default=64,
                   help="bytes; the server caps this at 65536 and defaults to 256")
    p.add_argument("--cpu", default="ee", choices=["ee", "iop"])

    p = sub.add_parser("read_string")
    p.add_argument("address", type=_int)

    p = sub.add_parser("disassemble")
    p.add_argument("address", type=_int, help="numeric only; 'pc' returns nothing")
    p.add_argument("--count", type=int, default=8)

    p = sub.add_parser("evaluate")
    p.add_argument("expression")

    p = sub.add_parser("set_breakpoint")
    p.add_argument("address", type=_int)
    p.add_argument("--condition", default=None)

    p = sub.add_parser("remove_breakpoint")
    p.add_argument("address", type=_int)

    # NOTE: the server takes a half-open [address, end) range, not a size.
    p = sub.add_parser("set_memcheck")
    p.add_argument("address", type=_int)
    p.add_argument("--size", type=int, default=4, help="converted to the server's `end`")
    p.add_argument("--type", default="write",
                   choices=["read", "write", "access", "onchange"])
    p.add_argument("--condition", default=None)

    p = sub.add_parser("remove_memcheck")
    p.add_argument("address", type=_int)
    p.add_argument("--size", type=int, default=4, help="converted to the server's `end`")

    p = sub.add_parser("raw", help="send a literal JSON command object")
    p.add_argument("json")

    args = ap.parse_args(argv)

    with DebugServer(args.host, args.port) as ds:
        if args.action == "raw":
            payload = json.loads(args.json)
            name = payload.pop("cmd")
            reply = ds.cmd(name, **payload)
        else:
            kw = {k: v for k, v in vars(args).items()
                  if k not in ("action", "host", "port") and v is not None}
            # memchecks take a half-open [address, end) range, not a size
            if "size" in kw:
                kw["end"] = kw["address"] + kw.pop("size")
                kw["end"] = f"0x{kw['end']:08x}"
            # the server wants hex strings for addresses
            if "address" in kw:
                kw["address"] = f"0x{kw['address']:08x}"
            reply = ds.cmd(args.action, **kw)

    json.dump(reply, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0 if reply.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
