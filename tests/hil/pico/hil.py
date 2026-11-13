#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""Hardware tests of an RP6502-PICO, run on a real machine through its
console and a debug probe. README.md describes the rig and each test."""

import argparse
import contextlib
import re
import socket
import sys
import tempfile
import threading
import time
from pathlib import Path

import rig
import roms

UPDATE_NAME = "hil-update.uf2"


class Fail(Exception):
    pass


class Rig:
    def __init__(self, args):
        self.args = args
        self.board = rig.Board(args.device, args.key)
        self.probe = rig.Probe(args.elf, args.probe) if args.elf else None
        self.tmp = tempfile.TemporaryDirectory()

    def rom(self, name):
        """The path of the ROM `name` of roms.ROMS, assembled on first use."""
        path = Path(self.tmp.name) / f"{name}.rp6502"
        if not path.exists():
            roms.ROMS[name]().write(path)
        return path

    def drive(self):
        """The drive of --dir as STATUS names it, such as MSC0."""
        name = self.args.dir.split(":")[0].upper()
        return "MSC" + name if name.isdigit() else name


class Watch:
    """Reads the audio load through the probe while the block runs. Without
    a probe, `rows` stays None."""

    def __init__(self, probe, xram_at=None):
        self.probe = probe
        self.xram_at = xram_at
        self.stop = threading.Event()
        self.rows = None
        self.error = None

    def __enter__(self):
        if self.probe:
            self.probe.__enter__()
            self.thread = threading.Thread(target=self._run)
            self.thread.start()
        return self

    def _run(self):
        try:
            self.rows = self.probe.watch(self.stop, self.xram_at)
        except Exception as e:
            self.error = e

    def __exit__(self, *_):
        if self.probe:
            self.stop.set()
            self.thread.join()
            self.probe.__exit__()
            if self.error:
                raise self.error


def check_load(rows):
    print(rig.load_summary(rows))
    if sum(r["overruns"] for r in rows):
        raise Fail("the audio interrupt overran")


def test_tput(r):
    """read_xram and write_xram speed to a USB drive, with every PSG voice,
    every OPL2 channel, and 48 kHz stereo PCM playing."""
    # On the reference drive of README.md, reads of 16 KB over 1100 KB/s
    # mean the whole read path is healthy.
    b = r.board
    b.wait_for_drive(r.drive())
    b.command(f"CD {r.args.dir}")
    rom = r.rom("hil-tput")
    b.upload(rom, rom.name)
    rows, end = 0, "no output for 20 s"
    with Watch(r.probe) as w:
        b.load(rom.name)
        for line in b.lines(20):
            if line is None:
                break
            if line == "done":
                end = None
                break
            print(line, flush=True)
            if line.startswith("?"):
                end = line[1:]
                break
            rows += bool(re.fullmatch(r" *\d+ +\d+ +\d+ +\(\d+ ms, \d+ ms\)", line))
    if line is None:
        b.stop()
    for name in (rom.name, roms.TPUT_FILE):
        try:
            b.command(f"UNLINK {name}")
        except RuntimeError:
            pass
    if end:
        raise Fail(end)
    if rows != len(roms.TPUT_SIZES):
        raise Fail(f"{rows} of {len(roms.TPUT_SIZES)} block sizes")
    if w.rows is not None:
        check_load(w.rows)


def test_term(r):
    """The console's terminal keeps up with the UART: the slowest things it
    does, each followed at once by a probe whose answer shows a lost byte."""
    b = r.board
    rom = r.rom("hil-term")
    t0, bad, end = time.monotonic(), [], "no output for 20 s"
    with contextlib.ExitStack() as stack:
        video = stack.enter_context(rig.LateLines(r.args.video)) if r.args.video else None
        time.sleep(1 if video else 0)
        b.run_in_ram(rom)
        for line in b.lines(20):
            if line is None:
                b.stop()
                break
            if line.startswith("?op"):
                bad.append(roms.TERM_OPS[int(line[4:], 16)][:24])
            if line.endswith("done"):
                end = None
                break
        print(f"{len(roms.TERM_OPS) * roms.TERM_LOOPS} probes in "
              f"{time.monotonic() - t0:.1f} s", flush=True)
    b.stop()
    if video:
        print(f"{video.late} of {video.frames} frames with late scanlines"
              + (f", first at frame {video.first[0]}" if video.first else ""), flush=True)
    if end:
        raise Fail(end)
    if bad:
        raise Fail(f"{len(bad)} probes lost bytes, after {sorted(set(map(repr, bad)))}")
    if video and (video.late or not video.frames):
        raise Fail("late scanlines" if video.late else "no video captured")


def test_audio(r):
    """The audio load of every sound engine at once with the bus busy, read
    through the probe. Needs --elf."""
    if not r.probe:
        raise Fail("needs --elf and a debug probe")
    b = r.board
    b.run_in_ram(r.rom("hil-audio"))
    time.sleep(2)
    with Watch(r.probe, roms.AUDIO_LOOPS) as w:
        time.sleep(r.args.seconds)
    b.stop()
    loops = sum((w.rows[i + 1]["xram"] - w.rows[i]["xram"]) & 0xFF
                for i in range(len(w.rows) - 1))
    print(f"{loops / max(1, len(w.rows) - 1):.1f} loops a second")
    check_load(w.rows)
    if not loops:
        raise Fail("the 6502 stopped")


def test_bus(r):
    """Reads and writes of the RIA registers by the 6502, checked."""
    b = r.board
    b.run_in_ram(r.rom("hil-bus"))
    time.sleep(r.args.seconds)
    b.stop()
    v = b.peek(0x10000 + roms.BUS_COUNTERS, 5)
    loops, errors = v[0] | v[1] << 8 | v[2] << 16, v[3] | v[4] << 8
    print(f"{loops} loops in {r.args.seconds} s, {errors} errors")
    if errors or not loops:
        raise Fail(f"{errors} errors in {loops} loops")


def second_login(host, port):
    """What a second telnet client gets."""
    try:
        s = socket.create_connection((host, port), 3)
    except OSError as e:
        return f"refused ({e.strerror or e})"
    s.settimeout(3)
    try:
        data = s.recv(64)
    except OSError:
        data = b""
    s.close()
    return f"connected, got {data!r}" if data else "refused (closed)"


def test_update(r):
    """The firmware update from a USB drive: FLASH of the UF2 from --uf2. A
    second telnet login during the update must be refused."""
    if not r.args.uf2:
        raise Fail("needs --uf2")
    b = r.board
    b.wait_for_drive(r.drive())
    b.command(f"CD {r.args.dir}")
    t0 = time.monotonic()
    b.upload(r.args.uf2, UPDATE_NAME)
    print(f"upload {Path(r.args.uf2).stat().st_size} bytes in "
          f"{time.monotonic() - t0:.1f} s", flush=True)
    b.write(f"FLASH {UPDATE_NAME}\r")
    t0, text, done, second = time.monotonic(), "", -1, None
    while done < 100:
        c = b.read(30)
        if not c:
            raise Fail(f"no output for 30 s at {done}%")
        text += c.decode("ascii", "replace")
        error = re.search(r"^\?.+", rig.clean(text), re.M)
        if error:
            raise Fail(error.group(0)[1:])
        found = re.findall(r"Flash (\d+)%", text)
        done = int(found[-1]) if found else -1
        if b.key and done >= 20 and second is None:
            second = second_login(*b.address())
            print(f"second login at {done}%: {second}", flush=True)
    print(f"100% after {time.monotonic() - t0:.1f} s", flush=True)
    b.close()
    time.sleep(2)
    b.reopen()
    print(next((l for l in b.status() if l.startswith("RIA")), "no RIA line"))
    b.wait_for_drive(r.drive())
    b.command(f"CD {r.args.dir}")
    b.command(f"UNLINK {UPDATE_NAME}")
    if second and not second.startswith("refused"):
        raise Fail(f"a second login got in: {second}")


TESTS = {name[5:]: fn for name, fn in globals().items() if name.startswith("test_")}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="tests:\n" + "\n".join(
            f"  {name:7} " + " ".join(fn.__doc__.split()) for name, fn in TESTS.items()))
    ap.add_argument("tests", nargs="*", metavar="test", help="the tests to run, in order")
    ap.add_argument("-c", "--config", metavar="file",
                    help="the settings file of rp6502.py, for the device and the key")
    ap.add_argument("-d", "--device", metavar="dev",
                    help="the serial port, or the address of the telnet console")
    ap.add_argument("-k", "--key", metavar="key", help="the telnet passkey")
    ap.add_argument("--elf", metavar="file",
                    help="the ELF of the firmware on the RIA; enables the probe")
    ap.add_argument("--probe", metavar="serial",
                    help="the serial number of the debug probe, when there are several")
    ap.add_argument("--flash", action="store_true",
                    help="program --elf through the probe before the tests")
    ap.add_argument("--dir", default="MSC0:/", metavar="dir",
                    help="the directory for test files, default MSC0:/")
    ap.add_argument("--uf2", metavar="file", help="the firmware for the update test")
    ap.add_argument("--seconds", type=int, default=20, metavar="n",
                    help="how long the audio and the bus tests run, default 20")
    ap.add_argument("--runs", type=int, default=1, metavar="n",
                    help="how many times to run the tests, default 1")
    ap.add_argument("--emit", metavar="dir", help="write the ROMs of the tests to dir")
    ap.add_argument("--video", metavar="dev",
                    help="a capture device on the VGA output, such as /dev/video0;"
                         " the term test then fails on a late scanline")
    a = ap.parse_args()
    unknown = [t for t in a.tests if t not in TESTS]
    if unknown:
        ap.error("unknown test: " + ", ".join(unknown))
    if a.flash and not a.elf:
        ap.error("--flash needs --elf")
    if a.emit:
        for name, make in roms.ROMS.items():
            make().write(Path(a.emit) / f"{name}.rp6502")
    if not a.tests and not a.flash:
        if not a.emit:
            ap.print_help()
        return 0
    if a.config:
        device, key = rig.settings(a.config)
        a.device, a.key = a.device or device, a.key or key

    r = Rig(a)
    if a.flash:
        r.board.stop()
        r.board.close()
        print(f"flash {a.elf}", flush=True)
        r.probe.flash()
        time.sleep(2)
        r.board.reopen()
    failed = 0
    for run in range(1, a.runs + 1):
        for name in a.tests:
            print(f"== {name}" + (f" run {run}" if a.runs > 1 else ""), flush=True)
            try:
                TESTS[name](r)
                print("PASS", flush=True)
            except (Fail, RuntimeError, TimeoutError, OSError) as e:
                print(f"FAIL: {e}", flush=True)
                failed += 1
    r.board.close()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
