#!/usr/bin/env python3
"""
pine.py — minimal PCSX2 PINE (IPC) client for RRV-Recomp ground-truthing.

PCSX2 exposes an IPC socket (PINE protocol) that lets us read EE/IOP memory of a
running (or savestate-loaded) session. We use it to compare our recompiled
runtime's intermediate buffers against PCSX2 as the behavioral reference.

Protocol, little-endian:
    request  = <u32 total_len><u8 opcode><args...>   (repeatable — see below)
    reply    = <u32 reply_len><u8 rc><value1><value2>...
rc == 0 means OK. Each value's width matches its opcode.

Opcodes (verified against `git show d5f75c9e4:pcsx2/PINE.cpp`, enum IPCCommand):
    MsgRead8=0 MsgRead16=1 MsgRead32=2 MsgRead64=3   args: <u32 addr>
    MsgWrite8=4 … MsgWrite64=7                       (not exposed here, see below)
    MsgVersion=8                                     args: none
    MsgSaveState=9  MsgLoadState=0xA                 args: <u8 slot>
    MsgTitle=0xB MsgID=0xC MsgUUID=0xD
    MsgGameVersion=0xE MsgStatus=0xF

BATCHING: PCSX2's ParseCommand() loops `while (buf_cnt < buf_size)`, so ONE
message may carry many commands; it replies with a single rc byte at offset 4
followed by every value concatenated in order. Batching a 924 KB .text
verification took ~0.1 s instead of minutes. Pipelining separate *messages*
does NOT work — PCSX2 services one message per recv.

This client is READ-ONLY by policy: the MsgWrite* opcodes exist in the
protocol but are deliberately not wrapped. Nothing in this project writes
guest memory over PINE, and a reference emulator that has been written to is
no longer a reference. Savestate save/load are exposed because they are how
deterministic anchors get captured (see SCENARIOS.md S-9).

Socket path: $TMPDIR/pcsx2.sock (macOS default used by tools/PCSX2.app).
PINE must be enabled first:
    EnablePINE = true  in  ~/Library/Application Support/PCSX2/inis/PCSX2.ini

IMPORTANT (project hygiene, HANDOFF §5): after any PINE session,
    pkill -f 'PCSX2.app/Contents/MacOS/PCSX2'
    restore EnablePINE = false
    rm -f $TMPDIR/pcsx2.sock

Nothing game-derived is embedded here — this is a debugging client only.
"""

import os
import socket
import struct
import sys

MSG_READ8, MSG_READ16, MSG_READ32, MSG_READ64 = 0, 1, 2, 3
MSG_VERSION = 8
MSG_SAVESTATE, MSG_LOADSTATE = 9, 0xA
_WIDTH = {MSG_READ8: 1, MSG_READ16: 2, MSG_READ32: 4, MSG_READ64: 8}

# A scratch savestate slot. The user's real anchors live in slots 1-6, so an
# automated capture must never target those (SCENARIOS.md S-9).
SCRATCH_SLOT = 10


def default_sock_path() -> str:
    tmp = os.environ.get("TMPDIR", "/tmp")
    return os.path.join(tmp, "pcsx2.sock")


class Pine:
    """One persistent connection to the PCSX2 PINE socket.

    Keeping the socket open across many reads is what makes savestate-anchored
    sampling cheap: load a state once, then hammer reads with no reconnect cost.
    """

    def __init__(self, sock_path=None, timeout=2.0):
        self.sock_path = sock_path or default_sock_path()
        self.timeout = timeout
        self.sock = None

    def connect(self):
        if not os.path.exists(self.sock_path):
            raise FileNotFoundError(
                f"{self.sock_path} not found — is PCSX2 running with EnablePINE=true?"
            )
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.sock_path)
        return self

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.close()

    def _read(self, opcode: int, addr: int) -> int:
        req = struct.pack("<IBI", 9, opcode, addr & 0xFFFFFFFF)
        self.sock.sendall(req)
        # reply: 4-byte length prefix, then 1-byte rc, then the value
        hdr = self._recv_exact(4)
        (reply_len,) = struct.unpack("<I", hdr)
        body = self._recv_exact(reply_len - 4)
        rc = body[0]
        if rc != 0:
            raise IOError(f"PINE read rc={rc} for addr=0x{addr:08X}")
        width = _WIDTH[opcode]
        val = body[1:1 + width]
        return int.from_bytes(val, "little")

    def _recv_exact(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise IOError("PINE socket closed mid-reply")
            buf += chunk
        return buf

    def read8(self, addr):  return self._read(MSG_READ8, addr)
    def read16(self, addr): return self._read(MSG_READ16, addr)
    def read32(self, addr): return self._read(MSG_READ32, addr)
    def read64(self, addr): return self._read(MSG_READ64, addr)

    def read_f32(self, addr) -> float:
        return struct.unpack("<f", struct.pack("<I", self.read32(addr)))[0]

    def read_qword_f32(self, addr):
        """Read a 16-byte qword as 4 floats (x,y,z,w) — the VU/matrix lane layout."""
        return [self.read_f32(addr + 4 * i) for i in range(4)]

    def read_bytes(self, addr, n):
        """Read n bytes, batched as u32 reads (was one round trip per byte)."""
        whole = n & ~3
        out = bytearray()
        for w in self.read_many([(MSG_READ32, addr + i) for i in range(0, whole, 4)]):
            out += w.to_bytes(4, "little")
        if n - whole:
            tail = self.read_many([(MSG_READ8, addr + i) for i in range(whole, n)])
            out += bytes(tail)
        return bytes(out)

    # --- batching ------------------------------------------------------------

    # PCSX2 reads one message at a time and its reply buffer is finite; keep
    # each message comfortably small rather than probing for the ceiling.
    _MAX_BATCH = 512

    def read_many(self, commands):
        """Issue many reads in as few round trips as possible.

        `commands` is a sequence of (opcode, addr). Returns values in the same
        order. This is the whole point of PINE for bulk work — a per-word round
        trip makes a full-RAM verification take minutes instead of ~0.1 s.
        """
        commands = list(commands)
        out = []
        for i in range(0, len(commands), self._MAX_BATCH):
            out.extend(self._read_batch(commands[i:i + self._MAX_BATCH]))
        return out

    def _read_batch(self, batch):
        if not batch:
            return []
        body = b"".join(
            struct.pack("<BI", op, addr & 0xFFFFFFFF) for op, addr in batch
        )
        self.sock.sendall(struct.pack("<I", 4 + len(body)) + body)
        (reply_len,) = struct.unpack("<I", self._recv_exact(4))
        payload = self._recv_exact(reply_len - 4)
        rc = payload[0]
        if rc != 0:
            raise IOError(f"PINE batch rc={rc} ({len(batch)} commands)")
        vals, off = [], 1
        for op, addr in batch:
            w = _WIDTH[op]
            if off + w > len(payload):
                raise IOError(
                    f"PINE batch reply truncated at 0x{addr:08X} "
                    f"({len(payload)} bytes for {len(batch)} commands)"
                )
            vals.append(int.from_bytes(payload[off:off + w], "little"))
            off += w
        return vals

    def read32_many(self, addrs):
        return self.read_many([(MSG_READ32, a) for a in addrs])

    def read_block32(self, addr, words):
        """Read `words` consecutive u32 starting at `addr`."""
        return self.read32_many([addr + 4 * i for i in range(words)])

    # --- savestates ----------------------------------------------------------

    def _slot_cmd(self, opcode: int, slot: int):
        if not 0 <= slot <= 255:
            raise ValueError(f"slot {slot} out of range")
        self.sock.sendall(struct.pack("<IBB", 6, opcode, slot))
        (reply_len,) = struct.unpack("<I", self._recv_exact(4))
        rc = self._recv_exact(reply_len - 4)[0]
        if rc != 0:
            raise IOError(f"PINE opcode 0x{opcode:X} slot {slot} failed rc={rc}")

    def save_state(self, slot: int = SCRATCH_SLOT):
        """Fire MsgSaveState. See the latency caveat below.

        PCSX2 dispatches this to the CPU thread and returns immediately, so the
        call is ASYNCHRONOUS: the guest keeps running for roughly a second
        while the file is written. Two consequences, both already paid for once:
          - wait for the slot file to stop growing before copying it out;
          - trust the coordinates you read back OUT of the saved image, not the
            predicate you triggered on.
        Defaults to the scratch slot so the user's anchors are never clobbered.
        """
        self._slot_cmd(MSG_SAVESTATE, slot)

    def load_state(self, slot: int = SCRATCH_SLOT):
        """Fire MsgLoadState. PCSX2 RESUMES after loading — it does not pause.

        Start sampling immediately; sleeping first measures a scene the anchor
        has already left (measured: 25 s past A2 moved scenePhase 2 -> 0x1F).
        Widen the window with [Framerate] NominalScalar (0.1 -> ~35 s).
        """
        self._slot_cmd(MSG_LOADSTATE, slot)

    def version(self) -> str:
        """MsgVersion — also a cheap liveness/handshake check."""
        self.sock.sendall(struct.pack("<IB", 5, MSG_VERSION))
        (reply_len,) = struct.unpack("<I", self._recv_exact(4))
        payload = self._recv_exact(reply_len - 4)
        if payload[0] != 0:
            raise IOError(f"PINE MsgVersion rc={payload[0]}")
        # <u32 size><char[size]>, NUL-terminated
        (size,) = struct.unpack_from("<I", payload, 1)
        return payload[5:5 + size].split(b"\0", 1)[0].decode("ascii", "replace")


# --- RRV-specific address helpers (USA ELF SLUS-20002; see HANDOFF §6) --------

VU1_DATA_BASE = 0x1100C000   # VU1 data memory: qword q lives at BASE + q*16
VU1_CODE_BASE = 0x11008000   # VU1 microcode

# Per-frame camera pipeline buffers (EE main RAM)
ADDR_PROJ        = 0x01DF06F0   # projection matrix
ADDR_VIEW_INV    = 0x01E24E80   # view-inverse (camera update-rate probe target)
ADDR_COMPOUND    = 0x01E24860   # compound viewinv x proj
ADDR_VU_STAGING  = 0x00348420   # VU-upload staging (q908-915 source)


def vu1_q(q: int) -> int:
    return VU1_DATA_BASE + q * 16


def dump_camera(p: "Pine"):
    """Structural snapshot of the camera pipeline — compare LANES vs PCSX2,
    not exact values (the camera moves between snapshots)."""
    print("proj q0      :", p.read_qword_f32(ADDR_PROJ))
    print("view_inv q0  :", p.read_qword_f32(ADDR_VIEW_INV))
    print("compound q0  :", p.read_qword_f32(ADDR_COMPOUND))
    print("VU1 q908     :", p.read_qword_f32(vu1_q(908)))
    print("VU1 q912     :", p.read_qword_f32(vu1_q(912)))


if __name__ == "__main__":
    # Usage:
    #   python3 pine.py                      -> dump camera snapshot
    #   python3 pine.py r32 0x01E24E80       -> read one u32
    #   python3 pine.py f32 0x01E24E80       -> read one float
    #   python3 pine.py qf  0x1100CE30       -> read a qword as 4 floats
    args = sys.argv[1:]
    with Pine() as p:
        if not args:
            dump_camera(p)
        else:
            op, addr = args[0], int(args[1], 0)
            if op == "r32":
                print(f"0x{p.read32(addr):08X}")
            elif op == "f32":
                print(p.read_f32(addr))
            elif op == "qf":
                print(p.read_qword_f32(addr))
            else:
                sys.exit(f"unknown op {op!r}")
