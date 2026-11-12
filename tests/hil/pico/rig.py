# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""The rig of the hardware tests: an RP6502-PICO, its console, and a debug
probe on the SWD port of the RIA."""

import configparser
import glob
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
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
        if self.key:
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
        buf, end = b"", time.monotonic() + timeout
        while b"\n]" not in buf:
            b = self.dev.read(1)
            buf += b
            if not b and time.monotonic() > end:
                raise TimeoutError(f"{line}: no prompt after {timeout} s")
        out = clean(buf[: buf.rindex(b"\n]")].decode("ascii", "replace"))
        lines = out.split("\n")[1:]
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


# The writes of watchdog_reboot(): SCRATCH4 cleared so the boot ROM ignores
# the scratch vector, PSM_WDSEL set for everything but the oscillators, and
# the TRIGGER bit of the watchdog's CTRL. The +0x2000 addresses are the
# atomic set aliases.
WATCHDOG_SCRATCH4 = 0x400D801C
PSM_WDSEL_SET = 0x40018008 + 0x2000
PSM_WDSEL_ALL_BUT_OSC = 0x01FFFFF3
WATCHDOG_CTRL_SET = 0x400D8000 + 0x2000
WATCHDOG_CTRL_TRIGGER = 1 << 31

# clk_sys of the RIA, the cycles that the audio interrupt counts in.
CLK_SYS_HZ = 256_000_000
# The rate of the audio interrupt, one OPL2 sample of 72 master clocks.
AUD_RATE = 49716


class Probe:
    """OpenOCD on a debug probe wired to the SWD port of the RIA. `elf` is
    the firmware on the RIA, for the addresses of its symbols."""

    TCL_PORT = 6666
    LOAD_FIELDS = ("second", "sum", "peak", "overruns", "peak_max", "pair",
                   "pair_max")

    def __init__(self, elf):
        self.elf = str(elf)
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
        argv += ["-f", "interface/cmsis-dap.cfg", "-f", target,
                 "-c", "adapter speed 5000"]
        for c in commands:
            argv += ["-c", c]
        return argv

    def flash(self):
        """Programs the ELF and restarts the RIA as the monitor's REBOOT
        does. Stop any ROM first: a restart while a ROM streams from a USB
        drive can hang the drive until it is replugged."""
        r = subprocess.run(self._argv("target/rp2350.cfg", [
            "program {%s} verify" % Path(self.elf).as_posix(), "poll off",
            f"mww {WATCHDOG_SCRATCH4:#x} 0",
            f"mww {PSM_WDSEL_SET:#x} {PSM_WDSEL_ALL_BUT_OSC:#x}",
            f"mww {WATCHDOG_CTRL_SET:#x} {WATCHDOG_CTRL_TRIGGER:#x}",
            "shutdown"]), capture_output=True, text=True, timeout=120)
        out = r.stdout + r.stderr
        if "Verified OK" not in out:
            raise RuntimeError("flash failed:\n" + out[-2000:])

    def __enter__(self):
        """Starts OpenOCD with only the memory access ports of the cores, so
        it never examines or halts a core, and connects to its Tcl port."""
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
