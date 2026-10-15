#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# A driver runs it, because only the host can say what time it is, and
# time_get must answer with that time.

import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rp6502_script  # noqa: E402
from rp6502_asm import (API_A, API_ERRNO, API_X, OP_ATTR_GET, OP_ATTR_SET,
                        OP_EXIT, OP_STRFTIME, OP_TIME_GET, OP_TIME_SET,
                        OP_WRITE_XSTACK, XSTACK, Asm, putc, putnib,
                        puthex)  # noqa: E402
from rp6502_rom import image  # noqa: E402

ATTR_ERRNO_OPT = 0x00
ATTR_CODE_PAGE = 0x02
ERRNO_LLVM = 2
STDOUT = 1

# 2025-07-01 12:00:00, a Tuesday, as the nine words strftime reads.
TM = (0, 0, 12, 1, 6, 125, 2, 181, 0)
# \x82 is é in code page 437.
FORMAT = "caf\x82 %z"
# 1735732800 is 2025-01-01 12:00:00 UTC.
SET_TIME = 1735732800
GOT = 0x0210


def putax(p):
    p.pha()
    p.txa()
    p.jsr_abs("puthex")
    p.pla()
    p.jmp_abs("puthex")


def prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex, putax)
    p.symbol("main")

    # The llvm-mos errno numbers, where EACCES is 13.
    p.pushl(ERRNO_LLVM)
    p.call_a(OP_ATTR_SET, ATTR_ERRNO_OPT)

    p.call_a(OP_ATTR_GET, ATTR_CODE_PAGE)
    p.pha()
    p.say("cp ")
    p.pla()
    p.jsr_abs("putax")
    p.say("\n")

    # The struct is pushed last byte first, so it reads upward in order.
    for b in reversed(struct.pack("<9h", *TM)):
        p.push(b)
    p.push_str(FORMAT)
    # The length strftime returns, then the text it left on the xstack.
    p.call(OP_STRFTIME)
    p.jsr_abs("putax")
    p.say(" ")
    p.call_a(OP_WRITE_XSTACK, STDOUT)
    p.say("\n")

    p.pushl(0)
    p.pushl(SET_TIME)
    p.call(OP_TIME_SET)
    p.pha()
    p.say("set ")
    p.pla()
    p.jsr_abs("putax")
    p.say(" ")
    p.lda_abs(API_ERRNO)
    p.ldx_abs(API_ERRNO + 1)
    p.jsr_abs("putax")
    p.say("\n")

    # The seconds are kept in RAM for the driver to compare with the host.
    p.call(OP_TIME_GET)
    p.pha()
    p.phx()
    p.ldx_imm(0)
    p.symbol("copy")
    p.lda_abs(XSTACK)
    p.sta_abx(GOT)
    p.inx()
    p.cpx_imm(8)
    p.bne("copy")
    p.say("get ")
    p.plx()
    p.pla()
    p.jsr_abs("putax")
    p.say("\n")

    p.pushl(850)
    p.call_a(OP_ATTR_SET, ATTR_CODE_PAGE)
    p.call_a(OP_ATTR_GET, ATTR_CODE_PAGE)
    p.pha()
    p.say("cp ")
    p.pla()
    p.jsr_abs("putax")
    p.say("\n")

    p.store(API_A, 0)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()
    return p


def drive(emu, rom, save_dir):
    def body(e):
        # The emulator's clock can be a few milliseconds behind Python's,
        # so a second is allowed below.
        before = int(time.time()) - 1
        e.cmd('wait "cp 01B5\\n"')
        # Ten code page bytes. Counted in UTF-8 it would be 000B.
        e.cmd('wait "000A café -0800\\n"')
        # Setting the time is refused with EACCES, because the clock is
        # the host's.
        e.cmd('wait "set FFFF 000D\\n"')
        e.cmd('wait "get 0000\\n"')
        after = int(time.time())
        got = struct.unpack("<q", bytes.fromhex(e.cmd("dump GOT 8")))[0]
        if not before <= got <= after:
            raise rp6502_script.ScriptError(
                f"time_get read {got}, not between {before} and {after}")
        # The code page the program set is gone when it runs again.
        e.cmd('wait "cp 0352\\n"')
        e.cmd("expect-exit 0")
        e.cmd("sys run")
        e.cmd('wait "cp 01B5\\n"')
        e.cmd("expect-exit 0")
    return rp6502_script.drive(emu, rom, body, args=("--cp", "437"),
                               env={"TZ": "PST8"}, save_dir=save_dir)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    ap.add_argument("--drive", action="store_true",
                    help="run the ROM on the emulator and check what it says")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the .rp6502 --emit wrote")
    ap.add_argument("--save-dir", help="the folder behind SAVE:")
    a = ap.parse_args()
    if a.emit:
        print(f"clk.rp6502 {image(prog()).write(a.emit, globals())} bytes")
    if a.drive:
        return drive(a.emu, a.rom, a.save_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
