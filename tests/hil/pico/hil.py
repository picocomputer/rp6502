#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""Hardware tests of an RP6502-PICO, run on a real machine through its
console and a debug probe. README.md describes the rig and each test."""

import argparse
import contextlib
import random
import re
import socket
import sys
import tempfile
import threading
import time
from pathlib import Path

import rig
import roms
import screen

import build

UPDATE_NAME = "hil-update.uf2"

# The keys of the hid test, by HID usage, and how many messages it times.
HID_KEY_A = 0x04
HID_KEY_BACKSPACE = 0x2A
HID_ROUND_TRIPS = 100

# The keys of the console tests, by HID usage, and the lines Desk.lay_out
# types: the tallest console's 32 rows and two to spare.
HID_KEY_C, HID_KEY_V, HID_KEY_X = 0x06, 0x19, 0x1B
# Under the quarter second in which the RIA counts a click into a run
# (VTERM_MOUSE_CLICK_MS).
CLICK_S = 0.2
HID_KEY_LEFT = 0x50
LAY_OUT_LINES = 34


class Fail(Exception):
    pass


class Rig:
    def __init__(self, args):
        self.args = args
        self.board = build.Board(args.device, args.key)
        self.probe = rig.Probe(args.elf, args.probe, self.board) if args.elf else None
        self.vga = (rig.VgaProbe(args.vga_elf, args.vga_probe, self.board)
                    if args.vga_elf else None)
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


def letters(count):
    """Letter keys, each different, picked afresh."""
    return random.sample(range(HID_KEY_A, HID_KEY_A + 26), count)


class Desk:
    """What someone at the machine has, for a test to use in their place: a
    keyboard and a mouse, which are on the debug probe of the RIA
    (build.RttHid), and the display, which --video captures and screen.py
    reads at the size the board says it shows, by --vga-elf or else --elf.
    Leaving the block unmounts the keyboard and the mouse, so no key or
    button stays down."""

    def __init__(self, r):
        if not r.args.video:
            raise Fail("needs --video")
        if not r.probe and not r.vga:
            raise Fail("needs --elf or --vga-elf")
        self.r = r
        self.offset = None

    def __enter__(self):
        r = self.r
        with contextlib.ExitStack() as stack:
            probe = stack.enter_context(r.probe or build.Probe(None, r.args.probe, r.board))
            eyes = stack.enter_context(r.vga) if r.vga else probe
            self.camera = screen.Camera(r.args.video, eyes)
            hid = stack.enter_context(build.RttHid(probe))
            self.keyboard, self.mouse = hid.keyboard(), hid.mouse()
            self.stack = stack.pop_all()
        return self

    def __exit__(self, *_):
        self.stack.close()

    def tap(self, *keys):
        """Presses the keys together and lets them go, at a typist's pace."""
        self.keyboard.tap(*keys, hold=0.02)
        time.sleep(0.03)

    def cancel(self):
        """Ctrl-C, which ends the line at the prompt without entering it."""
        self.tap(self.keyboard.LEFT_CTRL, HID_KEY_C)

    def point(self, x, y):
        """Puts the pointer on a pixel of the console's canvas. The mouse
        only moves by counts, two to a pixel, so it goes to the top left
        corner first, from wherever it is."""
        self.mouse.move(-4000, -4000)
        time.sleep(0.1)
        self.mouse.move(2 * x, 2 * y)
        time.sleep(0.1)

    def click(self, times):
        """Clicks the left button, three times for a triple click. The RIA
        counts a press into the run when it comes within a quarter second
        of the last release, and a message to the RIA can take longer than
        that. A run such a message may have broken is made again from its
        first click, so a slow bench is no fault of the firmware's."""
        for _ in range(5):
            up = None
            for _ in range(times):
                self.mouse.buttons(self.mouse.LEFT)
                late = up is not None and time.monotonic() - up > CLICK_S
                time.sleep(0.03)
                up = time.monotonic()
                self.mouse.buttons(0)
                if late:
                    break
            else:
                return
            time.sleep(0.3)
        raise Fail("the messages to the RIA are too slow for a run of clicks")

    def look(self, wait):
        """The screen as the capture has it `wait` seconds from now, and
        where the picture is in the capture when that is news."""
        time.sleep(wait)
        s = self.camera.look()
        if s.offset != self.offset:
            self.offset = s.offset
            print("the capture has the picture {:+g} pixels right and {:+g} down "
                  "of true".format(*s.offset), flush=True)
        return s

    def lay_out(self):
        """Fills the screen with one line on every row and the prompt on the
        last, and returns the line and the prompt's row. Each line is two
        letter keys and Ctrl-C, so nothing is entered. The letters are
        picked afresh each time, so what a test pastes can only be what it
        copied, never what an earlier run left on the clipboard. A row copied from this
        screen starts with the prompt's "]", which no command, ROM or
        address does, so a paste of it can only run an unknown command. The
        pointer waits at the right, off the lines."""
        self.point(600, 240)
        self.cancel()
        keys = letters(2)
        for _ in range(LAY_OUT_LINES):
            for key in keys:
                self.tap(key)
            self.cancel()
        s = self.look(0.5)
        line, last = s.line(0), len(s.text) - 1
        if not re.fullmatch(r"\]..\^C", line):
            raise Fail(f"the top row reads {line!r}, not a line the test typed")
        for y in range(1, last + 1):
            if s.line(y) != (line if y < last else "]"):
                raise Fail(f"row {y} reads {s.line(y)!r} under rows of {line!r}")
        return line, last


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


def check_console(r):
    """With a probe on each chip, the RIA's copy of the console's terminal
    and the VGA's are the same: sizes, cursors and rings (rig.console). The
    console is read once it is quiet, a few times, since the two chips are
    not read at one moment. Without both probes nothing is checked."""
    if not r.probe or not r.vga:
        return
    with r.probe, r.vga:
        for _ in range(10):
            time.sleep(0.2)
            bad = rig.console_differences(rig.console(r.probe), rig.console(r.vga))
            if not bad:
                print("the RIA's console and the VGA's are the same", flush=True)
                return
    raise Fail("the RIA's console is not the VGA's: " + "; ".join(bad))


def test_term(r):
    """The console's terminal keeps up with the UART: the slowest things it
    does, each followed at once by a probe whose answer shows a lost byte.
    With --vga-elf, a byte that came while the VGA's terminal had work
    pending, or a word the VGA's PIX FIFO had no room for, fails it too."""
    term_run(r, "hil-term")


def test_termload(r):
    """term, with two 8-bit bitmaps drawn over the whole canvas under the
    terminal, the most a program can load the VGA."""
    term_run(r, "hil-term-loaded")


def term_run(r, name):
    b = r.board
    rom = r.rom(name)
    t0, bad, end = time.monotonic(), [], "no output for 20 s"
    with contextlib.ExitStack() as stack:
        video = stack.enter_context(rig.LateLines(r.args.video)) if r.args.video else None
        vga = stack.enter_context(r.vga) if r.vga else None
        if vga:
            vga.clear()
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
        if vga:
            late, overrun = vga.faults()
            print(f"{late} bytes came during a job, PIX FIFO "
                  f"{'overran' if overrun else 'kept up'}", flush=True)
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
    if vga and (late or overrun):
        raise Fail(f"{late} bytes during a job" if late else "the PIX FIFO overran")
    check_console(r)


def test_sizes(r):
    """The RIA's copy of the console's terminal follows the VGA's through
    canvas and mode 0 changes, keeps its width when a program sets the line
    editor's, and answers the queries, or with 0x0D set does not."""
    b = r.board
    # The console has 32 rows on an SXGA display and 30 on the others.
    h = b"32" if "1280x1024" in " ".join(b.command("SET VGA")) else b"30"
    b.run_in_ram(r.rom("hil-sizes"))
    got, end = [], "no output for 20 s"
    for line in b.lines(20):
        if line is None:
            break
        if line.startswith("="):
            got.append(bytes.fromhex(line[1:]))
        if line.endswith("done"):
            end = None
            break
    b.stop()
    if end:
        raise Fail(end)
    want = [a.replace(b"{h}", h) for _, _, a in roms.SIZE_STEPS]
    bad = [f"{i}: {g!r} for {w!r}" for i, (g, w) in enumerate(zip(got, want)) if g != w]
    if len(got) != len(want):
        bad.append(f"{len(got)} answers of {len(want)}")
    if bad:
        raise Fail("; ".join(bad))


def test_select(r):
    """A selection made with the mouse on the console shows on the VGA as
    the RIA keeps it, at its place in the console's stream: as made, after
    output that scrolls it, and after output that scrolls it away. Needs
    --elf and --vga-elf, and someone at the mouse of the machine."""
    if not r.probe or not r.vga:
        raise Fail("needs --elf and --vga-elf")
    b = r.board
    check_console(r)
    with r.probe, r.vga:

        def same(when):
            time.sleep(0.5)
            ria, vga = rig.selection(r.probe), rig.selection(r.vga)
            print(f"{when}: {'shows' if ria['shows'] else 'none'}", flush=True)
            if ria != vga:
                raise Fail(f"{when}: the RIA has {ria}, the VGA {vga}")
            return ria

        b.command("STATUS")
        print("Drag across the text on the console with the mouse.", flush=True)
        end, last, still = time.monotonic() + 60, None, 0
        while still < 4:
            if time.monotonic() > end:
                raise Fail("no selection made in 60 s")
            now = rig.selection(r.probe)
            still = still + 1 if now["shows"] and now == last else 0
            last = now
            time.sleep(0.25)
        same("as made")
        b.command("STATUS")
        same("scrolled")
        for _ in range(8):
            b.command("STATUS")
        if same("scrolled away")["shows"]:
            raise Fail("the selection outlived its rows")


def test_hid(r):
    """A keyboard on the debug probe types a key at the monitor, which echoes
    it, and the time a message takes there and back. Needs a RIA built with
    RP6502_RTT_HID and a probe on it, and no --elf."""
    b = r.board
    b.stop()
    probe = r.probe or build.Probe(None, r.args.probe, b)
    with probe, build.RttHid(probe) as hid:
        keyboard = hid.keyboard()
        print(f"a keyboard is in hid slot {keyboard.slot}", flush=True)
        # What follows the prompt, such as its escapes, is not the echo.
        time.sleep(0.5)
        b.dev.flush_read_bufs()
        keyboard.tap(HID_KEY_A)
        echo = build.clean(b.read(2).decode("ascii", "replace"))
        print(f"the A key echoes {echo!r}", flush=True)
        keyboard.tap(HID_KEY_BACKSPACE)
        erased = b.read(2)
        if echo.lower() != "a":
            raise Fail(f"the A key echoed {echo!r}, and a US layout types a")
        if not erased:
            raise Fail("Backspace echoed nothing")
        times = sorted(hid.nop() * 1000 for _ in range(HID_ROUND_TRIPS))
        print(f"{len(times)} messages there and back: {times[0]:.1f} ms at least, "
              f"{times[len(times) // 2]:.1f} ms the median, {times[-1]:.1f} ms at most",
              flush=True)


def test_editkeys(r):
    """Keys of the line editor keep their order: at the monitor three
    letters are typed and the last two selected, then cut, a fourth letter
    and paste come with no wait between them, and the display has the first,
    the fourth, then the two that were cut. The letters are picked afresh,
    so the paste can only be this run's cut. Needs --video, --elf or
    --vga-elf, and a RIA built with RP6502_RTT_HID."""
    r.board.stop()
    with Desk(r) as desk:
        k = desk.keyboard
        _, last = desk.lay_out()
        try:
            a, b, c, d = letters(4)
            for key in (a, b, c):
                desk.tap(key)
            desk.tap(k.LEFT_SHIFT, HID_KEY_LEFT)
            desk.tap(k.LEFT_SHIFT, HID_KEY_LEFT)
            # Nothing waits between these three.
            k.tap(k.LEFT_CTRL, k.LEFT_SHIFT, HID_KEY_X)
            k.tap(d)
            k.tap(k.LEFT_CTRL, k.LEFT_SHIFT, HID_KEY_V)
            line = desk.look(0.6).line(last)
        finally:
            # Enter would run the line, which may be a hex address to the
            # monitor.
            desk.cancel()
    want = "]" + "".join(chr(ord("a") + key - HID_KEY_A) for key in (a, d, b, c))
    print(f"the prompt reads {line!r}", flush=True)
    # Caps Lock is the machine's, and its own keyboard may have it on.
    if line.lower() != want:
        raise Fail(f"the prompt reads {line!r}, and a US layout leaves {want!r}")


def test_pasteline(r):
    """A paste of one line waits for Enter: a triple click selects the top
    line of the screen, which the display shows highlighted, and copied and
    pasted it is at the prompt, not entered. Needs what editkeys needs."""
    r.board.stop()
    with Desk(r) as desk:
        k = desk.keyboard
        line, last = desk.lay_out()
        desk.point(3, 3)
        desk.click(3)
        s = desk.look(0.3)
        # The pointer is on the first cells of the line, which read as nothing.
        top = s.text[0]
        plain = sum(not lit and c != screen.UNREADABLE for c, lit in zip(top, s.reverse[0]))
        if plain or screen.UNREADABLE in top[4:]:
            raise Fail(f"the top line is not highlighted: {plain} of its cells are "
                       f"plain and {top.count(screen.UNREADABLE)} cannot be read")
        if any(any(row) for row in s.reverse[1:last]):
            raise Fail("more than the top line is highlighted")
        print("the top line is highlighted", flush=True)
        try:
            desk.tap(k.LEFT_CTRL, k.LEFT_SHIFT, HID_KEY_C)
            desk.tap(k.LEFT_CTRL, k.LEFT_SHIFT, HID_KEY_V)
            prompt = desk.look(0.8).line(last)
        finally:
            desk.cancel()
    print(f"the prompt reads {prompt!r}", flush=True)
    # A line that was entered has scrolled up, and left a bare prompt here.
    if prompt != "]" + line:
        raise Fail(f"the prompt reads {prompt!r}, and the paste of {line!r} "
                   f"leaves {']' + line!r} there")


def test_pastelines(r):
    """A paste of several lines enters all but the last: a drag from the top
    line of the screen into the second selects a line and part of one, and
    copied and pasted the first is entered, which the monitor answers, and
    the part waits at the next prompt. Needs what editkeys needs."""
    r.board.stop()
    with Desk(r) as desk:
        k = desk.keyboard
        line, last = desk.lay_out()
        desk.point(3, 3)
        desk.mouse.buttons(desk.mouse.LEFT)
        time.sleep(0.05)
        desk.mouse.move(60, 50)
        time.sleep(0.1)
        desk.mouse.move()
        time.sleep(0.05)
        desk.mouse.buttons(0)
        # The pointer goes off the text, so the cell the drag ended on reads.
        desk.mouse.move(800, 300)
        s = desk.look(0.3)
        part = "".join(c for c, lit in zip(s.text[1], s.reverse[1]) if lit)
        print(f"the drag highlights the top line and {part!r} of the second", flush=True)
        if not all(s.reverse[0][:len(line)]):
            raise Fail("the top line is not highlighted")
        if not part or part == line or not line.startswith(part):
            raise Fail(f"the second line's highlight is {part!r}, not a part of {line!r}")
        try:
            desk.tap(k.LEFT_CTRL, k.LEFT_SHIFT, HID_KEY_C)
            desk.tap(k.LEFT_CTRL, k.LEFT_SHIFT, HID_KEY_V)
            s = desk.look(1.0)
        finally:
            desk.cancel()
    got = [s.line(y) for y in range(last - 2, last + 1)]
    print(f"the last rows read {got}", flush=True)
    # The entered line is unknown to the monitor, whose answer is in the
    # words of the locale after a "?".
    if got[0] != "]" + line or not got[1].startswith("?") or got[2] != "]" + part:
        raise Fail(f"the last rows read {got}, and the paste leaves {']' + line!r}, "
                   f"the monitor's answer, and {']' + part!r}")


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
        error = re.search(r"^\?.+", build.clean(text), re.M)
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


def test_wide(r):
    """The display at 1280x720: a ROM puts the terminal on the 640x360
    canvas, which a 16:9 display (SET VGA 1) shows at that size, with a line
    on every row, and the capture reads each of them. Needs --video and
    --elf or --vga-elf."""
    if not r.args.video:
        raise Fail("needs --video")
    if not r.probe and not r.vga:
        raise Fail("needs --elf or --vga-elf")
    b = r.board
    b.run_in_ram(r.rom("hil-wide"))
    try:
        # The display takes a while to follow a change of its timing.
        time.sleep(4)
        with contextlib.ExitStack() as stack:
            eyes = stack.enter_context(r.vga or r.probe)
            s = screen.Camera(r.args.video, eyes).look()
    finally:
        b.stop()
    print("the capture has the picture {:+g} pixels right and {:+g} down of true, "
          "and {} rows".format(*s.offset, len(s.text)), flush=True)
    bad = [f"row {y + 1} reads {s.line(y)!r}" for y in range(roms.WIDE_ROWS)
           if s.line(y) != roms.wide_line(y + 1)]
    if len(s.text) != roms.WIDE_ROWS or bad:
        raise Fail("; ".join(bad[:3]) or f"{len(s.text)} rows")


TESTS = {name[5:]: fn for name, fn in globals().items() if name.startswith("test_")}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="tests:\n" + "\n".join(
            f"  {name:10} " + " ".join(fn.__doc__.split()) for name, fn in TESTS.items()))
    ap.add_argument("tests", nargs="*", metavar="test", help="the tests to run, in order")
    ap.add_argument("-c", "--config", metavar="file",
                    help="the settings file of rp6502.py, for the device and the key")
    ap.add_argument("-d", "--device", metavar="dev",
                    help="the serial port, the address of the telnet console, "
                         "or rtt or rtt:SERIAL for the console on a debug probe")
    ap.add_argument("-k", "--key", metavar="key", help="the telnet passkey")
    ap.add_argument("--elf", metavar="file",
                    help="the ELF of the firmware on the RIA; enables the probe")
    ap.add_argument("--probe", metavar="serial",
                    help="the serial number of the debug probe, when there are several")
    ap.add_argument("--vga-elf", metavar="file",
                    help="the ELF of the firmware on the VGA; term and termload "
                         "then read the VGA through its probe")
    ap.add_argument("--vga-probe", metavar="serial",
                    help="the serial number of the debug probe on the VGA")
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
                         " term and termload then fail on a late scanline, and"
                         " editkeys, pasteline and pastelines read the console"
                         " from it")
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
        device, key = build.settings(a.config)
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
