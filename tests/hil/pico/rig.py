# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""The rig of the hardware tests: an RP6502-PICO, its console, and a debug
probe on the SWD port of the RIA."""

import atexit
import configparser
import glob
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests" / "lib"))
sys.dont_write_bytecode = True

import rp6502  # noqa: E402

ESCAPE = re.compile(r"\x1b(\[[0-9;?>]*[A-Za-z]|[78])")


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


class RttDevice:
    """The console over a debug probe, as SEGGER RTT channel 0, in a firmware
    built with RP6502_RTT_DEBUG. It is a device of rp6502.Console, as the
    serial port and telnet are, named "rtt", or "rtt:SERIAL" to pick one of
    several probes.

    The firmware keeps a small structure in RAM with a ring of console output
    and a ring of console input; src/host/pico/rtt.h describes it. OpenOCD
    reads and writes them through the memory access port of the RP2350, as
    Probe does, so no core is examined or halted. The output ring holds 22 ms
    of a busy console and the firmware drops what does not fit, so a thread
    polls it all the time. OpenOCD's own RTT is not used: it has to attach to
    the cores, and it reads too seldom for a ring this small."""

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
        self.proc = None
        self.tcl = None
        self.tcl_lock = threading.Lock()
        self.tx = b""
        self.tx_lock = threading.Lock()
        self.thread = None
        self.near = self.far = None
        self.cb = 0
        self.stopping = False
        self.error = None

    def command(self, tcl):
        """Runs a Tcl command in OpenOCD and returns its result."""
        with self.tcl_lock:
            self.tcl.sendall(tcl.encode("ascii") + b"\x1a")
            buf = b""
            while not buf.endswith(b"\x1a"):
                chunk = self.tcl.recv(65536)
                if not chunk:
                    raise RuntimeError("OpenOCD closed its Tcl port")
                buf += chunk
        return buf[:-1].decode("ascii", "replace")

    def open(self):
        """Starts OpenOCD on the probe, finds the RTT structure, and starts
        the thread that carries the console."""
        openocd = find_openocd()
        scripts = Path(openocd).parent / "scripts"
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        argv = [openocd]
        if scripts.is_dir():
            argv += ["-s", str(scripts)]
        argv += ["-f", "interface/cmsis-dap.cfg"]
        if self.serial:
            argv += ["-c", f"adapter serial {self.serial}"]
        argv += ["-f", str(HERE / "memap.cfg"), "-c", "adapter speed 5000",
                 "-c", "gdb port disabled", "-c", "telnet port disabled",
                 "-c", f"tcl port {port}", "-c", "init", "-c", self.TCL]
        self.log = tempfile.TemporaryFile()
        self.proc = subprocess.Popen(argv, stdout=self.log, stderr=subprocess.STDOUT)
        # A script that dies must not leave OpenOCD holding the probe.
        atexit.register(self.close)
        end = time.monotonic() + 10
        try:
            while self.tcl is None:
                try:
                    self.tcl = socket.create_connection(("127.0.0.1", port), 2)
                except OSError:
                    if self.proc.poll() is not None or time.monotonic() > end:
                        self.log.seek(0)
                        raise RuntimeError("OpenOCD did not start on the probe:\n" +
                                           self.log.read().decode("ascii", "replace")[-600:])
                    time.sleep(0.1)
            self.tcl.settimeout(30)
            self.cb = int(self.command("rp6502_rtt_find %d %d" % self.RAM), 0)
            if not self.cb:
                raise RuntimeError("no RTT console in the chip on this probe; "
                                   "build its firmware with RP6502_RTT_DEBUG")
            # The ring holds the last of what was written with nobody
            # reading. The console starts from here.
            self.command("rp2350.ahb0 write_memory %d 32 [rp2350.ahb0 read_memory %d 32 1]" %
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
                text = self.command("rp6502_rtt_io %d {%s}" % (
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
        words = self.command("rp2350.ahb0 read_memory %d %d %d" % (addr, width, count)).split()
        if len(words) != count:
            raise RuntimeError(f"read of {addr:#x}: {' '.join(words)}")
        return [int(w, 16) for w in words]

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
        self.command("rp2350.ahb0 write_memory %d 32 {1}" % (self.cb + self.BREAK))

    def fileno(self):
        return self.near.fileno()

    def close(self):
        """Stops the thread and OpenOCD."""
        self.stopping = True
        if self.thread:
            self.thread.join(5)
            self.thread = None
        if self.tcl:
            try:
                self.tcl.sendall(b"shutdown\x1a")
            except OSError:
                pass
            self.tcl.close()
            self.tcl = None
        if self.proc:
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.proc = None
            self.log.close()
        for s in (self.near, self.far):
            if s:
                s.close()
        self.near = self.far = None


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
            self.dev = RttDevice(self.device.partition(":")[2])
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
        echo = line.encode("ascii") + b"\r\n"
        buf, end = b"", time.monotonic() + timeout
        while True:
            at = buf.find(echo)
            if at >= 0 and b"\n]" in buf[at + len(echo) - 1:]:
                break
            b = self.dev.read(1)
            buf += b
            if not b and time.monotonic() > end:
                raise TimeoutError(f"{line}: no prompt after {timeout} s")
        out = clean(buf[at + len(echo): buf.rindex(b"\n]") + 1].decode("ascii", "replace"))
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


# clk_sys of the RIA, the cycles that the audio interrupt counts in.
CLK_SYS_HZ = 256_000_000
# The rate of the audio interrupt, one OPL2 sample of 72 master clocks.
AUD_RATE = 49716


class Probe:
    """OpenOCD on a debug probe wired to the SWD port of the RIA. `elf` is
    the firmware on the RIA, for the addresses of its symbols. `serial` picks
    the probe by its serial number when there are several."""

    TCL_PORT = 6666
    LOAD_FIELDS = ("second", "sum", "peak", "overruns", "peak_max", "pair",
                   "pair_max")

    def __init__(self, elf, serial=None, board=None):
        self.elf = str(elf)
        self.serial = serial
        self.board = board
        self.sym = {}
        for name, value in rp6502.Symbols._elf_symbols(self.elf):
            self.sym.setdefault(name.split(".")[0], value)
        self.openocd = find_openocd()
        self.proc = None
        self.sock = None

    def _argv(self, target, commands):
        scripts = Path(self.openocd).parent / "scripts"
        argv = [self.openocd]
        if scripts.is_dir():
            argv += ["-s", str(scripts)]
        argv += ["-f", "interface/cmsis-dap.cfg"]
        if self.serial:
            argv += ["-c", f"adapter serial {self.serial}"]
        argv += ["-f", target, "-c", "adapter speed 5000"]
        for c in commands:
            argv += ["-c", c]
        return argv

    def flash(self):
        """Programs the ELF and reboots the chip cold, so it runs on its own
        with debug disabled. Stop any ROM first: a restart while a ROM
        streams from a USB drive can hang the drive until it is replugged.
        Nothing is written unless the chip already runs firmware of the ELF's
        name; src/host/pico/check.cfg reads it from the chip."""
        r = subprocess.run(self._argv("target/rp2350.cfg", [
            "source {%s}" % (ROOT / "src/host/pico/check.cfg").as_posix(),
            "rp6502_program {%s}" % Path(self.elf).as_posix()]),
            capture_output=True, text=True, timeout=120)
        out = r.stdout + r.stderr
        refused = re.search(r"^rp6502_check: .* Nothing was written\.$", out, re.M)
        if refused:
            raise RuntimeError(refused.group(0))
        if r.returncode or "Verified OK" not in out:
            raise RuntimeError("flash failed:\n" + out[-2000:])

    def console(self):
        """The board's RTT console when it is on this probe. Only one OpenOCD
        can hold a probe, so the reads then go through the console's."""
        dev = self.board.dev if self.board else None
        if isinstance(dev, RttDevice) and dev.serial == self.serial:
            return dev
        return None

    def __enter__(self):
        """Starts OpenOCD with only the memory access ports of the cores, so
        it never examines or halts a core, and connects to its Tcl port."""
        if self.console():
            return self
        self.log = tempfile.TemporaryFile()
        self.proc = subprocess.Popen(self._argv(str(HERE / "memap.cfg"), [
            "gdb_port disabled", "telnet_port disabled",
            f"tcl_port {self.TCL_PORT}", "init"]),
            stdout=self.log, stderr=subprocess.STDOUT)
        end = time.monotonic() + 10
        while self.sock is None:
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.TCL_PORT), 2)
            except OSError:
                if self.proc.poll() is not None or time.monotonic() > end:
                    self.__exit__()
                    raise RuntimeError("OpenOCD did not start")
                time.sleep(0.2)
        return self

    def __exit__(self, *_):
        if self.sock:
            self.sock.sendall(b"shutdown\x1a")
            self.sock.close()
            self.sock = None
        if self.proc:
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            if self.proc.returncode not in (0, None):
                self.log.seek(0)
                sys.stderr.write(self.log.read().decode("ascii", "replace")[-2000:])
            self.proc = None
            self.log.close()

    def read(self, addr, width, count):
        """`count` values of `width` bits from memory at `addr`."""
        if self.console():
            return self.console().read_memory(addr, width, count)
        self.sock.sendall(b"rp2350.ahb0 read_memory %d %d %d\x1a" % (addr, width, count))
        buf = b""
        while not buf.endswith(b"\x1a"):
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("OpenOCD closed its Tcl port")
            buf += chunk
        words = buf[:-1].split()
        if len(words) != count:
            raise RuntimeError(f"read of {addr:#x}: {buf[:-1].decode()}")
        return [int(w, 16) for w in words]

    def watch(self, stop, xram_at=None):
        """Reads aud_load, and the XRAM byte at `xram_at`, until `stop` is
        set. Returns a dict for each second that the audio interrupt
        published, without the first, which can start before the ROM."""
        rows = {}
        while not stop.is_set():
            v = self.read(self.sym["aud_load"], 32, len(self.LOAD_FIELDS))
            row = dict(zip(self.LOAD_FIELDS, v))
            if xram_at is not None:
                row["xram"] = self.read(self.sym["xram"] + xram_at, 8, 1)[0]
            rows.setdefault(row["second"], row)
            stop.wait(0.25)
        return list(rows.values())[1:]


def load_summary(rows):
    """One line of the audio load from the rows of Probe.sample()."""
    if not rows:
        return "audio load: no whole second sampled"
    avg = sum(r["sum"] for r in rows) / len(rows) / CLK_SYS_HZ * 100
    budget = CLK_SYS_HZ / AUD_RATE
    peak = max(r["peak"] for r in rows)
    return (f"audio load {avg:.1f}% over {len(rows)} s, peak sample "
            f"{peak} cycles ({peak / budget * 100:.0f}%), "
            f"{sum(r['overruns'] for r in rows)} overruns")


class LateLines:
    """Watches the VGA output on a capture device while the block runs and
    counts the frames with a late scanline: a row drawn in scanvideo's
    missing-line blue across most of the screen. Every frame is checked, so
    the capture must keep up at the display's rate. The test's own colors
    must not include that blue."""

    WIDTH, HEIGHT = 1280, 1024

    def __init__(self, device):
        self.device = device
        self.frames = 0
        self.late = 0
        self.first = None

    def __enter__(self):
        import threading
        self.proc = subprocess.Popen(
            ["ffmpeg", "-loglevel", "error", "-f", "v4l2", "-input_format", "yuyv422",
             "-video_size", f"{self.WIDTH}x{self.HEIGHT}", "-framerate", "60",
             "-i", self.device, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.thread = threading.Thread(target=self._run)
        self.thread.start()
        return self

    def _run(self):
        import numpy as np
        size = self.WIDTH * self.HEIGHT * 3
        while True:
            buf = self.proc.stdout.read(size)
            if len(buf) < size:
                return
            a = np.frombuffer(buf, np.uint8).reshape(self.HEIGHT, self.WIDTH, 3)
            blue = (a[:, :, 2] > 150) & (a[:, :, 0] < 90) & (a[:, :, 1] < 90)
            rows = np.count_nonzero(blue.sum(axis=1) > self.WIDTH // 2)
            self.frames += 1
            if rows:
                self.late += 1
                if self.first is None:
                    self.first = (self.frames, rows)

    def __exit__(self, *_):
        self.proc.terminate()
        self.thread.join()
        self.proc.wait()
