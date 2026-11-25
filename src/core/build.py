#!/usr/bin/env python3
#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

"""The Python helpers that the scripts which build and test a Picocomputer
share. The SDK's tools are in tools/.

The build runs every script through this file, as build.cmake's
RP6502_PYTHON does:

    python -X utf8 -B src/core/build.py SCRIPT [ARGS...]

which puts this folder, tools/ and tests/lib/ on the import path, so a script
imports build, rp6502 and the test libraries without changing sys.path
itself. A generator is run by hand the same way.
"""

import argparse
import atexit
import configparser
import glob
import importlib.util
import os
import re
import runpy
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TOOLS = ROOT / "tools"
TEST_LIB = ROOT / "tests" / "lib"

# What a script imports beside this file: rp6502 and the test libraries. The
# folders are put on the path here, ahead of the rest, which an embedded
# Python, such as the Pico VS Code extension's, starts with its standard
# library, and which holds no script's folder there.
for _folder in (TEST_LIB, TOOLS, HERE):
    if str(_folder) not in sys.path:
        sys.path.insert(0, str(_folder))

import rp6502  # noqa: E402


# ---- generated files ------------------------------------------------------
#
# A generated file has the same bytes on every host: UTF-8, with a bare LF
# ending each line, which Python's text mode would otherwise turn into CRLF
# on Windows.

def open_text(path):
    """A generated text file, opened for writing."""
    return open(path, "w", encoding="utf-8", newline="\n")


def write_text(path, text):
    """Writes a generated text file."""
    with open_text(path) as f:
        f.write(text)


# ---- images ---------------------------------------------------------------

PNG_CHANNELS = {1: 0, 2: 4, 3: 2, 4: 6}  # gray, gray+alpha, RGB, RGBA


def write_png(path, width, height, pixels, channels):
    """Writes an 8-bit PNG of width by height pixels, each channels bytes:
    1 gray, 2 gray and alpha, 3 RGB, or 4 RGBA."""
    stride = width * channels

    def chunk(tag, payload):
        crc = zlib.crc32(tag + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)

    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += pixels[y * stride:(y + 1) * stride]
    Path(path).write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8,
                                     PNG_CHANNELS[channels], 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b""))


# ---- generators -----------------------------------------------------------
#
# The command line every generator takes, which gen.cmake drives:
#
#   --list       one line per file: "in PATH" for each file the generator
#                reads, PATH from the repo root, then "KIND NAME" for each
#                file it writes
#   --c DIR      writes the C headers and sources into DIR
#   --sv DIR     writes the SystemVerilog packages into DIR
#
# A generator passes its inputs and, for each kind, the name of each file
# with the function that writes it.

KINDS = ("c", "sv")


def generate(inputs=(), c=None, sv=None, extra=None):
    """Returns the parsed arguments, for a generator with options of its
    own, which extra adds to the parser."""
    outputs = {"c": c or {}, "sv": sv or {}}
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    for kind in KINDS:
        ap.add_argument(f"--{kind}", metavar="DIR")
    if extra:
        extra(ap)
    args = ap.parse_args()
    if args.list:
        for path in inputs:
            print("in", path)
        for kind in KINDS:
            for name in outputs[kind]:
                print(kind, name)
    for kind in KINDS:
        out = getattr(args, kind)
        if out:
            for name, write in outputs[kind].items():
                write(Path(out) / name)
    return args


# ---- vendored modules -----------------------------------------------------

def load_module(path, name):
    """A Python file that is not part of a package, imported once under name,
    with its own folder on the import path for the files beside it."""
    if name in sys.modules:
        return sys.modules[name]
    sys.path.insert(0, str(Path(path).parent))
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


# ---- toolchain tools ------------------------------------------------------

def run(*cmd, input=None, cwd=None):
    """Runs a tool to the end and returns what it printed. The input goes in
    and the output comes back whole, so no pipe can fill and stall it."""
    return subprocess.run(cmd, input=input, cwd=cwd, capture_output=True,
                          text=True, check=True).stdout


def split_location(loc):
    """A "file:line" from addr2line or a compiler, as (file, line). The line
    is after the last colon, as a Windows file starts with a drive letter and
    a colon of its own. Line is None when the tool printed ?."""
    m = re.match(r"(.*):(\d+|\?)(?: \(discriminator \d+\))?$", loc)
    if not m:
        return loc, None
    return m.group(1), None if m.group(2) == "?" else int(m.group(2))


def same_path(path):
    """A path in the one spelling every comparison uses: absolute, links
    resolved, and in the host's case and separators."""
    return os.path.normcase(os.path.realpath(path))


# ---- the bench -------------------------------------------------------------
#
# A Picocomputer on the bench is reached through its console and through
# CMSIS-DAP debug probes on the SWD ports of its chips, which OpenOCD drives.
# The console is rp6502.py's, over a serial port, telnet, or RttDevice. A
# probe on the RIA is its keyboards, mice and gamepads too, through RttHid.

PICO = ROOT / "src" / "host" / "pico"


def find_openocd():
    """OpenOCD from $OPENOCD, the Pico extension of VS Code, or the PATH."""
    if os.environ.get("OPENOCD"):
        return os.environ["OPENOCD"]
    exe = "openocd.exe" if os.name == "nt" else "openocd"
    found = sorted(glob.glob(os.path.expanduser(f"~/.pico-sdk/openocd/*/{exe}")))
    if found:
        return found[-1]
    if shutil.which("openocd"):
        return shutil.which("openocd")
    raise RuntimeError("OpenOCD not found; set $OPENOCD")


def openocd_argv(serial, target, commands):
    """The command line of OpenOCD on a CMSIS-DAP probe, the one with
    `serial` when there are several, for the `target` file and then the
    commands."""
    openocd = find_openocd()
    scripts = Path(openocd).parent / "scripts"
    argv = [openocd]
    if scripts.is_dir():
        argv += ["-s", str(scripts)]
    argv += ["-f", "interface/cmsis-dap.cfg"]
    if serial:
        argv += ["-c", f"adapter serial {serial}"]
    argv += ["-f", str(target), "-c", "adapter speed 5000"]
    for c in commands:
        argv += ["-c", c]
    return argv


def flash(elf, serial=None):
    """Programs a Pico's firmware through the probe and reboots the chip
    cold, so it runs on its own with debug disabled. Nothing is written
    unless the chip already runs firmware of the ELF's name, which
    src/host/pico/check.cfg reads from the chip. Stop any ROM first: a
    restart while a ROM streams from a USB drive can hang the drive until it
    is replugged."""
    r = subprocess.run(openocd_argv(serial, "target/rp2350.cfg", [
        "source {%s}" % (PICO / "check.cfg").as_posix(),
        "rp6502_program {%s}" % Path(elf).as_posix()]),
        capture_output=True, text=True, timeout=120)
    out = r.stdout + r.stderr
    refused = re.search(r"^rp6502_check: .* Nothing was written\.$", out, re.M)
    if refused:
        raise RuntimeError(refused.group(0))
    if r.returncode or "Verified OK" not in out:
        raise RuntimeError("flash failed:\n" + out[-2000:])


class Turns:
    """A lock that is taken in the order it was asked for. A thread that
    polls with no pause, as the console on a probe does, takes a plain lock
    again before a waiting thread wakes, and can keep it waiting for half a
    second; here each waits for one command at most."""

    def __init__(self):
        self.ready = threading.Condition()
        self.asked = 0
        self.serving = 0

    def __enter__(self):
        with self.ready:
            turn = self.asked
            self.asked += 1
            self.ready.wait_for(lambda: self.serving == turn)

    def __exit__(self, *_):
        with self.ready:
            self.serving += 1
            self.ready.notify_all()


class OpenOCD:
    """OpenOCD on a probe, holding it until close. It reaches the RP2350
    through the memory access ports of its cores alone (memap.cfg), so it
    never examines, polls or halts a core, and takes Tcl commands, `tcl`
    first."""

    def __init__(self, serial=None, tcl=None):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        commands = ["gdb port disabled", "telnet port disabled",
                    f"tcl port {port}", "init"] + ([tcl] if tcl else [])
        self.log = tempfile.TemporaryFile()
        self.proc = subprocess.Popen(openocd_argv(serial, PICO / "memap.cfg", commands),
                                     stdout=self.log, stderr=subprocess.STDOUT)
        self.sock = None
        self.lock = Turns()
        # A script that dies must not leave OpenOCD holding the probe.
        atexit.register(self.close)
        end = time.monotonic() + 10
        while self.sock is None:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), 2)
            except OSError:
                if self.proc.poll() is not None or time.monotonic() > end:
                    text = self.tail()
                    self.close()
                    raise RuntimeError("OpenOCD did not start on the probe:\n" + text)
                time.sleep(0.1)
        self.sock.settimeout(30)

    def tail(self):
        """The end of what OpenOCD printed."""
        self.log.seek(0)
        return self.log.read().decode("ascii", "replace")[-2000:]

    def command(self, tcl):
        """Runs a Tcl command and returns its result."""
        with self.lock:
            self.sock.sendall(tcl.encode("ascii") + b"\x1a")
            buf = b""
            while not buf.endswith(b"\x1a"):
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise RuntimeError("OpenOCD closed its Tcl port")
                buf += chunk
        return buf[:-1].decode("ascii", "replace")

    def read_memory(self, addr, width, count):
        """`count` values of `width` bits from the chip's memory at `addr`."""
        words = self.command("rp2350.ahb0 read_memory %d %d %d" % (addr, width, count)).split()
        if len(words) != count:
            raise RuntimeError(f"read of {addr:#x}: {' '.join(words)}")
        return [int(w, 16) for w in words]

    def write_memory(self, addr, width, values):
        """Writes `values` of `width` bits to the chip's memory at `addr`."""
        text = self.command("rp2350.ahb0 write_memory %d %d {%s}" % (
            addr, width, " ".join(str(v) for v in values)))
        if text.strip():
            raise RuntimeError(f"write of {addr:#x}: {text.strip()}")

    def close(self):
        if self.sock:
            try:
                self.sock.sendall(b"shutdown\x1a")
            except OSError:
                pass
            self.sock.close()
            self.sock = None
        if self.proc:
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            if self.proc.returncode not in (0, None):
                sys.stderr.write(self.tail())
            self.proc = None
            self.log.close()


class RttDevice:
    """The console over a debug probe, as SEGGER RTT channel 0, in a firmware
    built with RP6502_RTT_STDIO. It is a device of rp6502.Console, as the
    serial port and telnet are, named "rtt", or "rtt:SERIAL" to pick one of
    several probes.

    The firmware keeps a small structure in RAM with a ring of console output
    and a ring of console input; src/host/pico/rtt.h describes it. OpenOCD
    reads and writes them through the memory access port of the RP2350, so no
    core is examined or halted. The output ring holds 22 ms of a busy console
    and the firmware drops what does not fit, so a thread polls it all the
    time. OpenOCD's own RTT is not used: it has to attach to the cores, and it
    reads too seldom for a ring this small."""

    RAM = (0x20000000, 0x20082000)
    # The firmware's own words after the RTT structure of one up ring and
    # one down ring: the bytes it dropped, and a break request.
    DROPPED = 72
    BREAK = 76
    # Console output kept for a reader that is behind; the oldest goes first.
    HELD_MAX = 1 << 20

    TCL = r"""
proc rp6502_rtt_find {lo hi} {
    for {set a $lo} {$a < $hi} {incr a 4096} {
        set i 0
        foreach v [rp2350.ahb0 read_memory $a 32 1024] {
            if {$v == 0x47474553} {
                set at [expr {$a + $i * 4}]
                lassign [rp2350.ahb0 read_memory $at 32 4] w0 w1 w2 w3
                if {$w1 == 0x52205245 && $w2 == 0x5454 && $w3 == 0} { return $at }
            }
            incr i
        }
    }
    return 0
}
proc rp6502_rtt_ring {buf size from to} {
    if {$to > $from} {
        return [rp2350.ahb0 read_memory [expr {$buf + $from}] 8 [expr {$to - $from}]]
    }
    set out [rp2350.ahb0 read_memory [expr {$buf + $from}] 8 [expr {$size - $from}]]
    if {$to > 0} { set out [concat $out [rp2350.ahb0 read_memory $buf 8 $to]] }
    return $out
}
proc rp6502_rtt_io {cb tx} {
    lassign [rp2350.ahb0 read_memory [expr {$cb + 28}] 32 4] ub us uw ur
    set out {}
    if {$uw != $ur && $uw < $us && $ur < $us} {
        set out [rp6502_rtt_ring $ub $us $ur $uw]
        rp2350.ahb0 write_memory [expr {$cb + 40}] 32 $uw
    }
    set sent 0
    set want [llength $tx]
    if {$want} {
        lassign [rp2350.ahb0 read_memory [expr {$cb + 52}] 32 4] db ds dw dr
        if {$dw < $ds && $dr < $ds} {
            set sent [expr {($dr + $ds - 1 - $dw) % $ds}]
            if {$sent > $want} { set sent $want }
            set first [expr {$ds - $dw}]
            if {$first > $sent} { set first $sent }
            if {$first > 0} {
                rp2350.ahb0 write_memory [expr {$db + $dw}] 8 [lrange $tx 0 [expr {$first - 1}]]
            }
            if {$sent > $first} {
                rp2350.ahb0 write_memory $db 8 [lrange $tx $first [expr {$sent - 1}]]
            }
            if {$sent > 0} {
                rp2350.ahb0 write_memory [expr {$cb + 60}] 32 [expr {($dw + $sent) % $ds}]
            }
        }
    }
    return "$sent $out"
}
"""

    def __init__(self, serial=None):
        self.serial = serial
        self.ocd = None
        self.tx = b""
        self.tx_lock = threading.Lock()
        self.thread = None
        self.near = self.far = None
        self.cb = 0
        self.stopping = False
        self.error = None

    def open(self):
        """Starts OpenOCD on the probe, finds the RTT structure, and starts
        the thread that carries the console."""
        self.ocd = OpenOCD(self.serial, self.TCL)
        try:
            self.cb = int(self.ocd.command("rp6502_rtt_find %d %d" % self.RAM), 0)
            if not self.cb:
                raise RuntimeError("no RTT console in the chip on this probe; "
                                   "build its firmware with RP6502_RTT_STDIO")
            # The ring holds the last of what was written with nobody
            # reading. The console starts from here.
            self.ocd.command("rp2350.ahb0 write_memory %d 32 [rp2350.ahb0 read_memory %d 32 1]" %
                             (self.cb + 40, self.cb + 36))
        except Exception:
            self.close()
            raise
        self.near, self.far = socket.socketpair()
        self.near.setblocking(False)
        self.far.setblocking(False)
        self.stopping = False
        self.error = None
        self.thread = threading.Thread(target=self.pump, daemon=True)
        self.thread.start()

    def pump(self):
        """Polls the rings: console output to the socket pair, and the bytes
        waiting in tx to the chip as its ring has room."""
        failures = 0
        held = bytearray()
        try:
            while not self.stopping:
                with self.tx_lock:
                    tx = self.tx[:48]
                text = self.ocd.command("rp6502_rtt_io %d {%s}" % (
                    self.cb, " ".join(str(b) for b in tx)))
                reply = text.split()
                if not reply or not reply[0].isdigit():
                    # A read the probe could not make, as while the chip
                    # resets. The poll is made again.
                    failures += 1
                    if failures > 200:
                        raise RuntimeError(f"OpenOCD: {text.strip()[:200]}")
                    time.sleep(0.01)
                    continue
                failures = 0
                sent = int(reply[0])
                if sent:
                    with self.tx_lock:
                        self.tx = self.tx[sent:]
                if len(reply) > 1:
                    held += bytes(int(v, 0) for v in reply[1:])
                # The socket pair takes what the reader has room for. The
                # rest is held here, so a reader that is slow, or is not
                # reading at all, never stops the polls.
                if held:
                    try:
                        del held[:self.far.send(held)]
                    except BlockingIOError:
                        pass
                    del held[:-self.HELD_MAX]
        except Exception as e:
            # The reader is on another thread: it sees the output end, and
            # write raises this.
            self.error = e
            self.far.close()

    def read_memory(self, addr, width, count):
        """`count` values of `width` bits from the chip's memory at `addr`."""
        return self.ocd.read_memory(addr, width, count)

    def write_memory(self, addr, width, values):
        """Writes `values` of `width` bits to the chip's memory at `addr`."""
        self.ocd.write_memory(addr, width, values)

    def dropped(self):
        """The bytes of console output the firmware has had no room for."""
        return self.read_memory(self.cb + self.DROPPED, 32, 1)[0]

    def write(self, data):
        """Queues data for the chip and returns once all of it is there."""
        with self.tx_lock:
            self.tx += data
        end = time.monotonic() + 30
        while True:
            with self.tx_lock:
                if not self.tx:
                    return
            if self.error:
                raise RuntimeError(f"RTT stopped: {self.error}")
            if time.monotonic() > end:
                raise TimeoutError("the chip did not take the console input")
            time.sleep(0.001)

    def read(self, size=1):
        """Up to `size` bytes, or fewer after RESPONSE_TIMEOUT of silence."""
        data = b""
        deadline = time.monotonic() + rp6502.RESPONSE_TIMEOUT
        while len(data) < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                chunk = self.near.recv(size - len(data))
                if not chunk:
                    break
                data += chunk
                deadline = time.monotonic() + rp6502.RESPONSE_TIMEOUT
            except BlockingIOError:
                if not select.select([self.near], [], [], remaining)[0]:
                    break
        return data

    def read_until(self, delimiter=b"\n"):
        start = time.monotonic()
        buffer = b""
        while delimiter not in buffer and time.monotonic() - start <= rp6502.RESPONSE_TIMEOUT:
            buffer += self.read(1)
        return buffer

    def flush_read_bufs(self):
        while True:
            try:
                if not self.near.recv(4096):
                    break
            except (BlockingIOError, OSError):
                break

    def send_break(self):
        """Asks the firmware for a break, which RTT has no signal for."""
        self.ocd.command("rp2350.ahb0 write_memory %d 32 {1}" % (self.cb + self.BREAK))

    def fileno(self):
        return self.near.fileno()

    def close(self):
        """Stops the thread and OpenOCD."""
        self.stopping = True
        if self.thread:
            self.thread.join(5)
            self.thread = None
        if self.ocd:
            self.ocd.close()
            self.ocd = None
        for s in (self.near, self.far):
            if s:
                s.close()
        self.near = self.far = None


class Probe:
    """A debug probe on the SWD port of a Pico, for reads and writes of its
    memory by symbol. `elf` is the firmware on the chip, or None when nothing
    is read by symbol, `serial` picks the probe when there are several, and
    `board` is the Board whose RttDevice may already hold this probe: only
    one OpenOCD can, so the reads and writes then go through it. They are
    made inside `with`."""

    def __init__(self, elf, serial=None, board=None):
        self.elf = elf and str(elf)
        self.serial = serial
        self.board = board
        self.sym = {}
        if elf:
            for name, value in rp6502.Symbols.elf_symbols(self.elf):
                self.sym.setdefault(name.split(".")[0], value)
        self.ocd = None

    def flash(self):
        """flash() of this probe's ELF."""
        flash(self.elf, self.serial)

    def console(self):
        """The board's RTT console when it is on this probe."""
        dev = self.board.dev if self.board else None
        if isinstance(dev, RttDevice) and dev.serial == self.serial:
            return dev
        return None

    def __enter__(self):
        if not self.console():
            self.ocd = OpenOCD(self.serial)
        return self

    def __exit__(self, *_):
        if self.ocd:
            self.ocd.close()
            self.ocd = None

    def read(self, addr, width, count):
        """`count` values of `width` bits from memory at `addr`."""
        return (self.console() or self.ocd).read_memory(addr, width, count)

    def write(self, addr, width, values):
        """Writes `values` of `width` bits to memory at `addr`."""
        (self.console() or self.ocd).write_memory(addr, width, values)


class RttHid:
    """Human input devices over a debug probe, in a RIA built with
    RP6502_RTT_HID: the bench mounts keyboards, mice and gamepads by their
    HID report descriptors and sends their reports, with nothing on the USB
    port. The RIA takes them as it takes USB devices, and knows no more of
    them than their descriptors say. src/host/pico/rtt_hid.h describes the
    messages.

    `probe` is a Probe on the RIA, and the devices are used inside its
    `with`, so they share the one OpenOCD that holds the probe with the RTT
    console and with the reads by symbol. Opening unmounts what an earlier
    run left mounted, and closing unmounts this run's.

    The firmware times nothing. A key held is a report, a wait here, and
    another report. Nothing of the emulator's script language is here."""

    ID = struct.unpack("<4I", b"RP6502 RTT HID\0\0")
    MOUNT, REPORT, UMOUNT, NOP = 1, 2, 3, 4
    # The drivers that take a device, HID_CLAIM_ in src/core/hid/hid.h.
    KEYBOARD, MOUSE, TABLET, PAD = 1, 2, 4, 8
    # How long the RIA has to take a message.
    TIMEOUT = 2.0

    def __init__(self, probe):
        self.probe = probe
        self.box = self.size = 0
        # Each seat as the last acknowledgment gave it: the hid slot of the
        # device mounted there, or None, and the drivers that took it.
        self.seats = []

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *_):
        self.close()

    def find(self):
        """The address of the firmware's structure, found in RAM by its id,
        or 0."""
        lo, hi = RttDevice.RAM
        for addr in range(lo, hi, 4096):
            # Three words of the next page too, for an id across the two.
            words = self.probe.read(addr, 32, 1024 + (3 if addr + 4096 < hi else 0))
            at = -1
            while self.ID[0] in words[at + 1:1024]:
                at = words.index(self.ID[0], at + 1)
                if tuple(words[at:at + 4]) == self.ID:
                    return addr + at * 4
        return 0

    def open(self):
        """Finds the structure, waits out a message an earlier run left, and
        unmounts every device still mounted."""
        block = self.find()
        if not block:
            raise RuntimeError("no HID devices in the chip on this probe; "
                               "build the RIA's firmware with RP6502_RTT_HID")
        self.box, sizes = self.probe.read(block + 16, 32, 2)
        self.size, self.seats = sizes & 0xFFFF, [(None, 0)] * (sizes >> 16)
        self.wait("a message left from before")
        # What an earlier run left mounted goes as this run's will.
        self.close()

    def close(self):
        """Unmounts every device."""
        for seat, (slot, _) in enumerate(self.seats):
            if slot is not None:
                self.send(self.UMOUNT, seat)

    def poll(self):
        """Reads the head, which is 0 once the RIA has taken the message,
        and the seats after it."""
        n = len(self.seats)
        words = self.probe.read(self.box + self.size + 4, 32, 1 + n // 2)
        table = struct.pack("<%dI" % (n // 2), *words[1:])
        self.seats = [(table[i] - 1 if table[i] else None, table[n + i]) for i in range(n)]
        return words[0]

    def wait(self, what):
        """Polls until the RIA has taken the message, or raises."""
        end = time.monotonic() + self.TIMEOUT
        while self.poll():
            if time.monotonic() > end:
                raise TimeoutError(f"the RIA did not take {what} in {self.TIMEOUT} s")

    @staticmethod
    def pack(verb, seat=0, data=b"", vendor_id=0, product_id=0):
        """The words of a message: its bytes, which end where the ids begin
        and start on a word, then the ids, then the head."""
        raw = bytes(-len(data) % 4) + bytes(data) + struct.pack(
            "<HHI", vendor_id, product_id, verb | seat << 8 | len(data) << 16)
        return struct.unpack("<%dI" % (len(raw) // 4), raw)

    def send(self, verb, seat=0, data=b"", vendor_id=0, product_id=0):
        """Sends one message and returns when the RIA has acted on it. The
        seats are then as that left them."""
        if len(data) > self.size:
            raise ValueError(f"{len(data)} bytes do not fit a message of {self.size}")
        words = self.pack(verb, seat, data, vendor_id, product_id)
        # The head is the last word written. A long message goes in several
        # writes, so the console's polls get their turns at the probe.
        at = self.box + self.size + 8 - len(words) * 4
        for i in range(0, len(words), 32):
            self.probe.write(at + i * 4, 32, words[i:i + 32])
        self.wait("the message")

    def nop(self):
        """Sends a message that does nothing and returns the seconds it took
        the RIA to acknowledge it."""
        start = time.monotonic()
        self.send(self.NOP)
        return time.monotonic() - start

    def mount(self, descriptor, vendor_id=0, product_id=0, device=None):
        """Mounts a device from its HID report descriptor and its USB ids, as
        when one is plugged in, and returns it: a `device`, or a HidDevice."""
        device = device or HidDevice
        free = [seat for seat, (slot, _) in enumerate(self.seats) if slot is None]
        if not free:
            raise RuntimeError(f"all {len(self.seats)} seats are taken")
        self.send(self.MOUNT, free[0], descriptor, vendor_id, product_id)
        slot, claims = self.seats[free[0]]
        if slot is None:
            raise RuntimeError("the RIA did not mount the device: no driver "
                               "took its descriptor, or has room for it")
        mounted = device(self, free[0])
        if claims & device.CLAIMS != device.CLAIMS:
            mounted.unmount()
            raise RuntimeError(f"the RIA mounted the device for drivers {claims:#x} "
                               f"and not {device.CLAIMS:#x}")
        return mounted

    def keyboard(self):
        """Mounts a standard keyboard."""
        return self.mount(HidKeyboard.DESCRIPTOR, device=HidKeyboard)

    def mouse(self):
        """Mounts a mouse."""
        return self.mount(HidMouse.DESCRIPTOR, device=HidMouse)

    def gamepad(self):
        """Mounts a gamepad, which is the player of the lowest number that
        has none."""
        return self.mount(HidGamepad.DESCRIPTOR, device=HidGamepad)


class HidDevice:
    """A device RttHid.mount() mounted, in its seat until unmount(). `slot`
    is its hid slot in the RIA, and `claims` the drivers that took it."""

    # The drivers a device of this class must have been taken by.
    CLAIMS = 0

    def __init__(self, hid, seat):
        self.hid = hid
        self.seat = seat
        self.slot, self.claims = hid.seats[seat]

    def report(self, data):
        """Sends a report as the device sends it over USB, which is with its
        report id first when the descriptor has ids."""
        self.hid.send(RttHid.REPORT, self.seat, data)
        if self.hid.seats[self.seat][0] != self.slot:
            raise RuntimeError(f"the device in seat {self.seat} is gone; "
                               "the RIA has rebooted")

    def unmount(self):
        """Unmounts the device, as when one is unplugged."""
        self.hid.send(RttHid.UMOUNT, self.seat)


class HidKeyboard(HidDevice):
    """A standard keyboard, with the boot keyboard's report: a bit for each
    modifier and six keys. A key is its HID usage, as src/core/hid/usage.h
    names them: 0x04 is the key a US keyboard has A on, and what it types
    is the RIA's layout's to say."""

    CLAIMS = RttHid.KEYBOARD
    (LEFT_CTRL, LEFT_SHIFT, LEFT_ALT, LEFT_GUI,
     RIGHT_CTRL, RIGHT_SHIFT, RIGHT_ALT, RIGHT_GUI) = range(0xE0, 0xE8)

    DESCRIPTOR = bytes([
        0x05, 0x01,        # Usage Page (Generic Desktop)
        0x09, 0x06,        # Usage (Keyboard)
        0xA1, 0x01,        # Collection (Application)
        0x05, 0x07,        # Usage Page (Keyboard)
        0x19, 0xE0,        # Usage Minimum (Left Control)
        0x29, 0xE7,        # Usage Maximum (Right GUI)
        0x15, 0x00,        # Logical Minimum (0)
        0x25, 0x01,        # Logical Maximum (1)
        0x75, 0x01,        # Report Size (1)
        0x95, 0x08,        # Report Count (8)
        0x81, 0x02,        # Input (Data, Variable, Absolute)
        0x95, 0x01,        # Report Count (1)
        0x75, 0x08,        # Report Size (8)
        0x81, 0x01,        # Input (Constant)
        0x95, 0x05,        # Report Count (5)
        0x75, 0x01,        # Report Size (1)
        0x05, 0x08,        # Usage Page (LEDs)
        0x19, 0x01,        # Usage Minimum (Num Lock)
        0x29, 0x05,        # Usage Maximum (Kana)
        0x91, 0x02,        # Output (Data, Variable, Absolute)
        0x95, 0x01,        # Report Count (1)
        0x75, 0x03,        # Report Size (3)
        0x91, 0x01,        # Output (Constant)
        0x95, 0x06,        # Report Count (6)
        0x75, 0x08,        # Report Size (8)
        0x15, 0x00,        # Logical Minimum (0)
        0x26, 0xFF, 0x00,  # Logical Maximum (255)
        0x05, 0x07,        # Usage Page (Keyboard)
        0x19, 0x00,        # Usage Minimum (0)
        0x2A, 0xFF, 0x00,  # Usage Maximum (255)
        0x81, 0x00,        # Input (Data, Array)
        0xC0,              # End Collection
    ])

    def __init__(self, hid, seat):
        super().__init__(hid, seat)
        self.keys = []

    @staticmethod
    def pack(keys):
        """The report with `keys` down."""
        modifiers = sum(1 << (k - 0xE0) for k in keys if 0xE0 <= k <= 0xE7)
        codes = [k for k in keys if not 0xE0 <= k <= 0xE7]
        if len(codes) > 6:
            raise ValueError("this keyboard reports six keys and the modifiers")
        return bytes([modifiers, 0] + codes + [0] * (6 - len(codes)))

    def down(self, *keys):
        """Presses the keys, which stay down."""
        held = self.keys + [k for k in keys if k not in self.keys]
        report = self.pack(held)
        self.keys = held
        self.report(report)

    def up(self, *keys):
        """Lets the keys go, or every key."""
        self.keys = [k for k in self.keys if keys and k not in keys]
        self.report(self.pack(self.keys))

    def tap(self, *keys, hold=0):
        """Presses the keys together, modifiers among them, and lets them go
        after `hold` seconds."""
        self.down(*keys)
        time.sleep(hold)
        self.up(*keys)


class HidMouse(HidDevice):
    """A mouse with five buttons, a wheel, and a pan. It moves the mouse of
    a program and, as a USB mouse does, the pointer of the tablet and the
    console. Two counts are one of the mouse block of a program."""

    CLAIMS = RttHid.MOUSE | RttHid.TABLET
    LEFT, RIGHT, MIDDLE, BACK, FORWARD = 1, 2, 4, 8, 16

    DESCRIPTOR = bytes([
        0x05, 0x01,        # Usage Page (Generic Desktop)
        0x09, 0x02,        # Usage (Mouse)
        0xA1, 0x01,        # Collection (Application)
        0x09, 0x01,        # Usage (Pointer)
        0xA1, 0x00,        # Collection (Physical)
        0x05, 0x09,        # Usage Page (Button)
        0x19, 0x01,        # Usage Minimum (1)
        0x29, 0x05,        # Usage Maximum (5)
        0x15, 0x00,        # Logical Minimum (0)
        0x25, 0x01,        # Logical Maximum (1)
        0x95, 0x05,        # Report Count (5)
        0x75, 0x01,        # Report Size (1)
        0x81, 0x02,        # Input (Data, Variable, Absolute)
        0x95, 0x01,        # Report Count (1)
        0x75, 0x03,        # Report Size (3)
        0x81, 0x01,        # Input (Constant)
        0x05, 0x01,        # Usage Page (Generic Desktop)
        0x09, 0x30,        # Usage (X)
        0x09, 0x31,        # Usage (Y)
        0x16, 0x01, 0x80,  # Logical Minimum (-32767)
        0x26, 0xFF, 0x7F,  # Logical Maximum (32767)
        0x75, 0x10,        # Report Size (16)
        0x95, 0x02,        # Report Count (2)
        0x81, 0x06,        # Input (Data, Variable, Relative)
        0x09, 0x38,        # Usage (Wheel)
        0x15, 0x81,        # Logical Minimum (-127)
        0x25, 0x7F,        # Logical Maximum (127)
        0x75, 0x08,        # Report Size (8)
        0x95, 0x01,        # Report Count (1)
        0x81, 0x06,        # Input (Data, Variable, Relative)
        0x05, 0x0C,        # Usage Page (Consumer)
        0x0A, 0x38, 0x02,  # Usage (AC Pan)
        0x95, 0x01,        # Report Count (1)
        0x81, 0x06,        # Input (Data, Variable, Relative)
        0xC0,              # End Collection
        0xC0,              # End Collection
    ])

    def __init__(self, hid, seat):
        super().__init__(hid, seat)
        self.held = 0

    @staticmethod
    def pack(buttons, dx=0, dy=0, wheel=0, pan=0):
        """The report: the buttons down and what moved since the last."""
        return struct.pack("<Bhhbb", buttons, dx, dy, wheel, pan)

    def move(self, dx=0, dy=0, wheel=0, pan=0):
        """Moves right and down by counts, and the wheel up and the pan
        right by notches."""
        self.report(self.pack(self.held, dx, dy, wheel, pan))

    def buttons(self, mask):
        """Holds the buttons of `mask` down, and no others."""
        self.held = mask
        self.move()


class HidGamepad(HidDevice):
    """A gamepad with sixteen buttons, a hat for the dpad, two sticks and
    two triggers. The buttons and the dpad are the bits a program reads in
    BTN0, BTN1 and DPAD, a stick is -128 to 127, and a trigger 0 to 255."""

    CLAIMS = RttHid.PAD
    A, B, C, X, Y, Z, L1, R1, L2, R2, SELECT, START, HOME, L3, R3 = (
        1 << i for i in range(15))
    UP, DOWN, LEFT, RIGHT = 1, 2, 4, 8
    # The hat of each dpad: clockwise from north, and 8 at rest.
    HAT = {0: 8, 1: 0, 9: 1, 8: 2, 10: 3, 2: 4, 6: 5, 4: 6, 5: 7}

    DESCRIPTOR = bytes([
        0x05, 0x01,        # Usage Page (Generic Desktop)
        0x09, 0x05,        # Usage (Gamepad)
        0xA1, 0x01,        # Collection (Application)
        0x05, 0x09,        # Usage Page (Button)
        0x19, 0x01,        # Usage Minimum (1)
        0x29, 0x10,        # Usage Maximum (16)
        0x15, 0x00,        # Logical Minimum (0)
        0x25, 0x01,        # Logical Maximum (1)
        0x75, 0x01,        # Report Size (1)
        0x95, 0x10,        # Report Count (16)
        0x81, 0x02,        # Input (Data, Variable, Absolute)
        0x05, 0x01,        # Usage Page (Generic Desktop)
        0x09, 0x39,        # Usage (Hat Switch)
        0x15, 0x00,        # Logical Minimum (0)
        0x25, 0x07,        # Logical Maximum (7)
        0x35, 0x00,        # Physical Minimum (0)
        0x46, 0x3B, 0x01,  # Physical Maximum (315)
        0x65, 0x14,        # Unit (Degrees)
        0x75, 0x04,        # Report Size (4)
        0x95, 0x01,        # Report Count (1)
        0x81, 0x42,        # Input (Data, Variable, Absolute, Null State)
        0x65, 0x00,        # Unit (None)
        0x95, 0x01,        # Report Count (1)
        0x81, 0x01,        # Input (Constant)
        0x09, 0x30,        # Usage (X)
        0x09, 0x31,        # Usage (Y)
        0x09, 0x32,        # Usage (Z)
        0x09, 0x35,        # Usage (Rz)
        0x09, 0x33,        # Usage (Rx)
        0x09, 0x34,        # Usage (Ry)
        0x26, 0xFF, 0x00,  # Logical Maximum (255)
        0x46, 0xFF, 0x00,  # Physical Maximum (255)
        0x75, 0x08,        # Report Size (8)
        0x95, 0x06,        # Report Count (6)
        0x81, 0x02,        # Input (Data, Variable, Absolute)
        0xC0,              # End Collection
    ])

    def __init__(self, hid, seat):
        super().__init__(hid, seat)
        self.state = {}

    @classmethod
    def pack(cls, buttons=0, dpad=0, lx=0, ly=0, rx=0, ry=0, lt=0, rt=0):
        """The report: the buttons, the hat, the sticks, and the triggers."""
        return struct.pack("<HB6B", buttons, cls.HAT[dpad],
                           lx + 128, ly + 128, rx + 128, ry + 128, lt, rt)

    def set(self, **changes):
        """Changes any of buttons, dpad, lx, ly, rx, ry, lt and rt, and
        sends the whole state, which with no change sends it again."""
        self.state.update(changes)
        self.report(self.pack(**self.state))


class Board:
    """One console session with the monitor of the RIA, over telnet when
    there is a key, or over a serial port."""

    def __init__(self, device, key):
        self.device = device or rp6502.Console.default_device()
        self.key = key
        self.dev = None
        self.con = None
        self.open()

    def address(self):
        """The host and the port of the telnet console."""
        host, _, port = self.device.rpartition(":")
        if host and port.isdigit():
            return host, int(port)
        return self.device, 23

    def open(self):
        if self.device == "rtt" or self.device.startswith("rtt:"):
            self.dev = RttDevice(self.device.partition(":")[2] or None)
        elif self.key:
            self.dev = rp6502.TelnetDevice(*self.address(), self.key)
        else:
            self.dev = rp6502.SerialDevice(self.device)
        self.con = rp6502.Console(self.dev)
        self.con.send_break()

    def close(self):
        if self.dev:
            self.dev.close()
        self.dev = self.con = None

    def reopen(self, timeout=90):
        """Waits for the RIA to come back from a reboot and opens the console
        again. A telnet session ends with the reboot. A serial port stays open
        and answers a break once the monitor runs."""
        end = time.monotonic() + timeout
        while True:
            try:
                if self.key or not self.dev:
                    self.close()
                    self.open()
                else:
                    self.con.send_break()
                return
            except (OSError, RuntimeError, TimeoutError):
                if time.monotonic() > end:
                    raise RuntimeError(
                        f"the RIA did not come back within {timeout} s; "
                        "reboot it by hand")
                time.sleep(2)

    def write(self, text):
        self.dev.write(text.encode("ascii"))

    def read(self, silence):
        """The next bytes of output, or b"" after `silence` seconds without
        any."""
        end = time.monotonic() + silence
        while True:
            b = self.dev.read(1)
            if b:
                return b
            if time.monotonic() > end:
                return b""
            time.sleep(0.01)

    def lines(self, silence):
        """The lines of output, cleaned, until `silence` seconds pass without
        output. The last item is None when the silence ended it."""
        buf = b""
        while True:
            b = self.read(silence)
            if not b:
                yield None
                return
            buf += b
            if b == b"\n":
                yield clean(buf.decode("ascii", "replace")).rstrip("\n")
                buf = b""

    def command(self, line, timeout=10):
        """Runs a monitor command and returns its output lines. A line that
        starts with "?" is an error of the monitor and raises."""
        self.dev.flush_read_bufs()
        self.write(line + "\r")
        # The output is what follows the monitor's echo of the line, up to
        # the next prompt. A prompt still on its way from before the line was
        # sent, as when a break lands on a program that just ended, is ahead
        # of the echo and is not taken for the end of the output.
        # The line editor turns bracketed paste off as the line ends, so its
        # escape can come between the last newline and the prompt.
        echo = line.encode("ascii") + b"\r\n"
        buf, end = b"", time.monotonic() + timeout
        while True:
            at = buf.find(echo)
            prompt = at >= 0 and PROMPT.search(buf, at + len(echo) - 1)
            if prompt:
                break
            b = self.dev.read(1)
            buf += b
            if not b and time.monotonic() > end:
                raise TimeoutError(f"{line}: no prompt after {timeout} s")
        out = clean(buf[at + len(echo): prompt.start() + 1].decode("ascii", "replace"))
        lines = out.split("\n")[:-1]
        for text in lines:
            if text.startswith("?"):
                raise RuntimeError(f"{line}: {text}")
        return lines

    def status(self):
        return self.command("STATUS")

    def wait_for_drive(self, drive, timeout=30):
        """Waits until STATUS lists the drive, such as MSC0."""
        end = time.monotonic() + timeout
        while not any(l.startswith(drive + ":") for l in self.status()):
            if time.monotonic() > end:
                raise RuntimeError(f"{drive} is missing; replug the drive")
            time.sleep(2)

    def upload(self, path, name):
        with open(path, "rb") as f:
            self.con.upload(f, name)

    def load(self, name):
        """Loads a ROM from the current directory. The 6502 starts."""
        self.con.load(name)

    def run_in_ram(self, path):
        """Writes a ROM straight into memory with BINARY and starts the 6502.
        The ROM must not need a drive."""
        rom = rp6502.ROM()
        rom.add_rom_file(str(path))
        self.con.send_rom(rom)
        self.con.reset()

    def stop(self):
        """Stops the 6502 and returns to the monitor."""
        self.con.send_break()

    def peek(self, addr, count):
        """Bytes of memory read by the monitor. XRAM is at $10000-$1FFFF."""
        out = []
        while len(out) < count:
            line = self.command(f"{addr + len(out):X}")[0]
            out += [int(v, 16) for v in line.split("|")[0].split()[1:]]
        return out[:count]


ESCAPE = re.compile(r"\x1b(\[[0-9;?>]*[A-Za-z]|[78])")

# The monitor's prompt at the start of a line, after any escapes.
PROMPT = re.compile(rb"\n(?:\x1b\[[0-9;?>]*[A-Za-z])*\]")


def clean(text):
    """The text of console output without escape sequences, carriage
    returns, and the characters that backspaces erase."""
    text = ESCAPE.sub("", text).replace("\r", "")
    return re.sub(r".?\x08", "", text)


def settings(path):
    """The device and the key from the [RP6502][PICO] section of the
    settings file of rp6502.py, or from its older [RP6502][Launch]."""
    config = configparser.ConfigParser(interpolation=None)
    config.SECTCRE = re.compile(r"\[(?P<header>.+)\]")
    if not config.read(path):
        raise RuntimeError(f"cannot read {path}")
    for name in ("RP6502][PICO", "RP6502][Launch"):
        if config.has_section(name):
            return config[name].get("device") or None, config[name].get("key") or None
    raise RuntimeError(f"{path} has no [RP6502][PICO] section")


# ---- running a script -----------------------------------------------------

def main():
    if len(sys.argv) < 2:
        sys.exit("usage: build.py SCRIPT [ARGS...]")
    script = os.path.abspath(sys.argv[1])
    sys.argv = sys.argv[1:]
    sys.path.insert(0, os.path.dirname(script))
    runpy.run_path(script, run_name="__main__")


if __name__ == "__main__":
    main()
