#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

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
# COM_WIRE_HOLD_S is COM_WIRE_HOLD_MS from core/sys/com.h, converted to seconds.
COM_WIRE_HOLD_S = 5.0
# rln_read_line writes its terminal queries between \x1b[?25l and \x1b[?25h.
HANDSHAKE = re.compile(rb"\x1b\[\?25l.*?\x1b\[\?25h", re.S)


class Board:
    def __init__(self, a):
        self.device = a.device
        self.telnet = a.telnet
        self.key = a.key

    def has_telnet(self):
        return bool(self.telnet and self.key)

    def serial(self):
        con = fresh(Console(SerialDevice(self.device)))
        con.held = False  # the UART has no flow control, so a Ctrl-C is read at once
        return con

    def telnet_con(self):
        host, _, port = self.telnet.rpartition(":")
        if not (host and port.isdigit()):
            host, port = self.telnet, "23"
        con = fresh(Console(TelnetDevice(host, int(port), self.key)))
        con.held = True  # telnet input is not read while its ring is full, for up to COM_WIRE_HOLD_MS
        return con


def fresh(con):
    """The line editor sets line_end to the CR or LF that ends a line, and a
    break does not clear it, so a CR or LF sent first after the break can be
    discarded as the second half of a CR and LF pair. The x clears line_end
    and the DEL erases the x."""
    con.send_break()
    drain(con, 0.4)
    send(con, b"x\x7f", 0.3)
    return con


def drain(con, secs):
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
    con.upload(io.BytesIO(image(prog).to_bytes()), NAME)
    con.load(NAME)


def exits():
    """A read of $FFE0 moves a byte staged for the 6502, when there is one,
    into $FFE2. Since the program exits without reading $FFE2, the monitor
    gets that byte only through com_rx_reclaim."""
    p = Asm()
    p.bit_abs(0xFFE0)
    p.store(API_A, 0)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()
    return p


def loops():
    p = Asm()
    p.symbol("loop")
    p.jmp_abs("loop")
    return p


def waits_for_sigint():
    p = Asm()
    p.symbol("poll")
    p.bit_abs(0xFFF0)
    with p.branch("bvc"):
        p.store(0xFFE1, ord("@"))
        p.stp()
    p.jmp_abs("poll")
    return p


def echoes():
    p = Asm()
    p.symbol("poll")
    p.bit_abs(0xFFE0)
    with p.branch("bvc"):
        p.lda_abs(0xFFE2)
        p.sta_abs(0xFFE1)
    p.jmp_abs("poll")
    return p


def line_ends(open_con):
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
    con = open_con()
    try:
        out = send(con, b"0000\rABCDE\r", 1.6)
        check(prompts(out) == 2 and b"ABCDE" in out,
              f"{prompts(out)} prompt(s), echo {plain(out)[-40:]!r}")
    finally:
        con.serial.close()


def type_ahead_survives_a_program_start(open_con):
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
    con = open_con()
    try:
        program(con, loops())
        time.sleep(0.5)
        drain(con, 0.3)
        con.send_break()
        check(prompts(send(con, b"\r", 1.0)) >= 1, "no prompt after the break")
    finally:
        con.serial.close()


def a_ctrl_c_reaches_a_program_that_never_reads(open_con):
    con = open_con()
    try:
        program(con, waits_for_sigint())
        send(con, b"f" * 40, 0.5)
        out = send(con, b"\x03", 1.0)
        held = b"@" not in out
        if held:
            out += drain(con, COM_WIRE_HOLD_S + 1.0)
        check(b"@" in out, f"no '@' after {COM_WIRE_HOLD_S + 2.5:.0f}s: {plain(out)[:60]!r}")
        check(held == con.held, f"{'held' if held else 'at once'}, expected {'held' if con.held else 'at once'}")
    finally:
        con.send_break()
        con.serial.close()


def a_register_reader_echoes_in_order(open_con):
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
    a_ctrl_c_reaches_a_program_that_never_reads,
    a_register_reader_echoes_in_order,
    a_large_upload_is_intact,
]


def two_sources_do_not_interleave(board):
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
        except Exception as e:
            failed += 1
            print(f"FAIL  {name}: {e}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
