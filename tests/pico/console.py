#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The console, on the board it was written for.
#
# Everything here is a claim the desktop suites cannot make: a real UART with
# a hardware FIFO and a break line, a telnet session over the radio, the 6502
# reading the console through its registers on the other core, and two
# consoles typing at once. The board is driven through tools/rp6502.py, the
# tool a developer uses, never through a path of this file's own. Every
# program run here is assembled below and uploaded first, so nothing depends
# on what the SD card happens to hold.
#
# It needs a board on a serial port and, for the two-console claims, the
# telnet passkey. CI has neither; a desktop tree registers this only when
# RP6502_DEVICE names the port.
#
#   python3 tests/pico/console.py --device /dev/ttyACM0 \
#       --telnet 192.168.1.89 --key word

import argparse
import glob
import io
import os
import re
import select
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
sys.path.insert(0, os.path.join(HERE, "..", "gen"))
from rp6502 import Console, SerialDevice, TelnetDevice  # noqa: E402
from rp6502_asm import API_A, API_X, Asm  # noqa: E402
from rp6502_rom import image  # noqa: E402

OP_EXIT = 0xFF
NAME = "console_test.rp6502"
# The line editor's handshake: the prompt's cursor-position and device queries,
# which a terminal answers and this file does not. Elided so an echo reads.
HANDSHAKE = re.compile(rb"\x1b\[\?25l.*?\x1b\[\?25h", re.S)


class Board:
    def __init__(self, a):
        self.device = a.device
        self.telnet = a.telnet
        self.key = a.key

    def has_telnet(self):
        return bool(self.telnet and self.key)

    def serial(self):
        return fresh(Console(SerialDevice(self.device)))

    def telnet_con(self):
        host, _, port = self.telnet.rpartition(":")
        if not (host and port.isdigit()):
            host, port = self.telnet, "23"
        return fresh(Console(TelnetDevice(host, int(port), self.key)))


def fresh(con):
    """A console at the monitor with nothing pending. A break returns the
    monitor but the line editor keeps what it knew about the wire -- the
    half of a CR LF pair still owed -- so a printable clears that and a
    DEL takes the printable back, leaving a clean gate and an empty line."""
    con.send_break()
    drain(con, 0.4)
    send(con, b"x\x7f", 0.3)
    return con


def drain(con, secs):
    """Everything the board says for secs, answering nothing."""
    out = b""
    end = time.monotonic() + secs
    while time.monotonic() < end:
        r, _, _ = select.select([con.serial], [], [], 0.05)
        if r:
            out += con.serial.read(1)
    return out


def send(con, data, secs=1.2):
    con.serial.write(data)
    return drain(con, secs)


def prompts(out):
    return out.count(b"]")


def plain(out):
    return HANDSHAKE.sub(b"<P>", out)


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def flash(elf, device):
    """The repo's own Flash task: openocd over the CMSIS-DAP probe on the RIA,
    then wait for the console port to enumerate again. Command line only --
    no build variable may reflash a board by accident."""
    found = sorted(glob.glob(os.path.expanduser("~/.pico-sdk/openocd/*/openocd")))
    if not found:
        raise SystemExit("no openocd under ~/.pico-sdk")
    ocd = found[-1]
    subprocess.run(
        [ocd, "-s", os.path.join(os.path.dirname(ocd), "scripts"),
         "-f", "interface/cmsis-dap.cfg", "-f", "target/rp2350.cfg",
         "-c", f"adapter speed 5000; program {elf} verify reset exit"],
        check=True, capture_output=True)
    for _ in range(100):
        time.sleep(0.1)
        if os.path.exists(device):
            break
    time.sleep(2.0)


def program(con, prog):
    """Upload a program and run it, the way rp6502.py run does."""
    con.upload(io.BytesIO(image(prog).to_bytes()), NAME)
    con.load(NAME)


def exits():
    """Polls the console once and leaves, the way a C runtime's startup does.
    The poll is the point: a read of $FFE0 moves the byte staged for the
    6502 into $FFE2, and a program that then leaves without reading it has
    committed a byte out of the console that the monitor must get back."""
    p = Asm()
    p.bit_abs(0xFFE0)
    p.store(API_A, 0)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()
    return p


def loops():
    """Runs and never reads the console."""
    p = Asm()
    p.symbol("loop")
    p.jmp_abs("loop")
    return p


def echoes():
    """Reads the console through its registers: polls bit 6 of $FFE0 and
    copies $FFE2 to $FFE1, which is what a program that skips the OS does."""
    p = Asm()
    p.symbol("poll")
    p.bit_abs(0xFFE0)
    with p.branch("bvc"):
        p.lda_abs(0xFFE2)
        p.sta_abs(0xFFE1)
    p.jmp_abs("poll")
    return p


# ---- one console ----------------------------------------------------------


def line_ends(open_con):
    """Either spelling ends a line, CR LF is one line end, and a blank line
    between two of them survives."""
    cases = [
        ("CR", b"0000\r", 1),
        ("LF", b"0000\n", 1),
        ("CRLF", b"0000\r\n", 1),
        ("LFCR", b"0000\n\r", 1),
        ("CR CR", b"0000\r\r", 2),
        ("blank between", b"0000\r\n\r\n0001\r\n", 3),
    ]
    for name, data, want in cases:
        con = open_con()
        try:
            got = prompts(send(con, data, 1.4))
            check(got == want, f"{name}: {got} prompt(s), want {want}")
        finally:
            con.serial.close()


def type_ahead_survives_a_command(open_con):
    """A command that runs the 6502 to read RAM does not eat what was typed
    behind it."""
    con = open_con()
    try:
        out = send(con, b"0000\rABCDE\r", 1.6)
        check(prompts(out) == 2 and b"ABCDE" in out,
              f"{prompts(out)} prompt(s), echo {plain(out)[-40:]!r}")
    finally:
        con.serial.close()


def type_ahead_survives_a_program_start(open_con):
    """A program start does not eat what was typed behind it. A byte the
    6502's first $FFE0 poll commits into $FFE2 has to reach the monitor's
    line editor when the program leaves."""
    con = open_con()
    try:
        program(con, exits())
        drain(con, 1.0)
        out = send(con, b'LOAD "%s"\rABCDEFGH\r' % NAME.encode(), 3.0)
        check(b"ABCDEFGH" in out,
              f"echo {re.findall(rb'[A-H]{2,}', plain(out))!r}")
    finally:
        con.serial.close()


def a_break_reaches_a_program_that_never_reads(open_con):
    """The break line is watched whether or not the program reads: the UART
    FIFO is drained every pass, not only on a read, or a break in it would
    never be seen. send_break raises if the monitor does not come back."""
    con = open_con()
    try:
        program(con, loops())
        time.sleep(0.5)
        drain(con, 0.3)
        con.send_break()
        check(prompts(send(con, b"\r", 1.0)) >= 1, "no prompt after the break")
    finally:
        con.serial.close()


def a_register_reader_echoes_in_order(open_con):
    """A program reading $FFE2 behind bit 6 of $FFE0 sees every byte, once,
    in order, from the console the bytes were typed at."""
    con = open_con()
    try:
        program(con, echoes())
        time.sleep(0.5)
        drain(con, 0.5)
        out = send(con, b"the quick brown fox", 1.5)
        check(out == b"the quick brown fox", f"echoed {out!r}")
        con.send_break()
    finally:
        con.serial.close()


def a_large_upload_is_intact(open_con):
    """Four kilobytes over the console, every chunk's CRC checked by the
    monitor, across the line editor's own line ends."""
    con = open_con()
    try:
        data = bytes((i * 7 + (i >> 5)) & 0xFF for i in range(4096))
        con.upload(io.BytesIO(data), "console_test.bin")
    finally:
        con.serial.close()


PER_CONSOLE = [
    line_ends,
    type_ahead_survives_a_command,
    type_ahead_survives_a_program_start,
    a_break_reaches_a_program_that_never_reads,
    a_register_reader_echoes_in_order,
    a_large_upload_is_intact,
]


# ---- two consoles ---------------------------------------------------------


def two_sources_do_not_interleave(board):
    """A burst on one console is not sliced by a burst on the other: the
    picker holds a source until it has been dry for the dwell."""
    ser = board.serial()
    net = board.telnet_con()
    try:
        drain(ser, 0.3)
        for trial in range(3):
            send(ser, b"\r", 0.4)
            drain(net, 0.2)
            a = threading.Thread(target=net.serial.write, args=(b"A" * 24,))
            b = threading.Thread(target=ser.serial.write, args=(b"b" * 24,))
            a.start()
            b.start()
            a.join()
            b.join()
            time.sleep(0.6)
            line = send(ser, b"\r", 1.2).split(b"\r\n")[0]
            body = re.sub(rb"[^Ab]", b"", line)
            runs = [m.group(0) for m in re.finditer(rb"(.)\1*", body)]
            check(len(body) == 48 and len(runs) == 2,
                  f"trial {trial}: {[r[:1] + b'x%d' % len(r) for r in runs]}")
    finally:
        ser.serial.close()
        net.serial.close()


def a_client_that_leaves_mid_crlf_owes_the_next_one_nothing(board):
    """A telnet client that drops between the CR and the LF of its Enter
    does not cost the next client its first Enter."""
    a = board.telnet_con()
    send(a, b"0000\r", 1.0)
    a.serial.close()
    time.sleep(1.0)
    b = board.telnet_con()
    try:
        out = send(b, b"0001\n", 2.0)
        check(prompts(out) == 1 and b"0001" in out,
              f"{prompts(out)} prompt(s), {plain(out)[:60]!r}")
    finally:
        b.serial.close()


TWO_CONSOLES = [
    two_sources_do_not_interleave,
    a_client_that_leaves_mid_crlf_owes_the_next_one_nothing,
]


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", default="/dev/ttyACM0", help="the RIA's console port")
    ap.add_argument("--telnet", metavar="HOST[:PORT]", help="the board's address, for the second console")
    ap.add_argument("--key", help="the telnet passkey")
    ap.add_argument("-k", dest="only", metavar="NAME", help="only the tests whose name contains this")
    ap.add_argument("--flash", metavar="ELF", help="flash this image over the debug probe first")
    a = ap.parse_args()
    if a.flash:
        flash(a.flash, a.device)
    board = Board(a)

    consoles = [("serial", board.serial)]
    if board.has_telnet():
        consoles.append(("telnet", board.telnet_con))
    else:
        print("no --telnet and --key: the two-console claims are skipped")

    tests = [(f"{name}.{fn.__name__}", (lambda fn=fn, o=opener: fn(o)))
             for name, opener in consoles for fn in PER_CONSOLE]
    if board.has_telnet():
        tests += [(fn.__name__, (lambda fn=fn: fn(board))) for fn in TWO_CONSOLES]

    failed = 0
    for name, run in tests:
        if a.only and a.only not in name:
            continue
        try:
            run()
            print(f"ok    {name}")
        except Exception as e:  # a claim that did not hold, or a board that did not answer
            failed += 1
            print(f"FAIL  {name}: {e}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
