#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

from rp6502_asm import (API_A, API_X, OP_ARGV, OP_ATTR_GET, OP_ATTR_SET,
                        OP_DROP_XSTACK, OP_EXEC, OP_EXIT, XSTACK, Asm, putc,
                        putnib, puthex)
from rp6502_rom import main, rom, script

ATTR_LAUNCHER = 0x06
ATTR_EXIT_CODE = 0x07
CHILD = ":child.rp6502"
CHILD_EXIT = 7
WAITER = ":waiter.rp6502"


def exit_with(p, code):
    p.store(API_A, code)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()


def launcher_prog(child=CHILD):
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex)
    p.symbol("main")

    # The launcher is never given arguments, so ARGV returns a table of four
    # bytes and then argv[0] alone.
    p.say("launcher ")
    p.call(OP_ARGV)
    p.ldx_imm(4)
    p.symbol("table")
    p.lda_abs(XSTACK)
    p.dex()
    p.bne("table")
    p.symbol("argv0")
    p.lda_abs(XSTACK)
    with p.branch("beq"):
        p.jsr_abs("putc")
        p.bra("argv0")
    p.say("\n")

    p.call_a(OP_ATTR_GET, ATTR_LAUNCHER)
    p.tax()
    p.beq("first")
    # Both bytes of the exit code, high first.
    p.say("back ")
    p.call_a(OP_ATTR_GET, ATTR_EXIT_CODE)
    p.pha()
    p.txa()
    p.jsr_abs("puthex")
    p.pla()
    p.jsr_abs("puthex")
    p.say("\n")
    exit_with(p, 0)

    p.symbol("first")
    p.pushl(1)
    p.call_a(OP_ATTR_SET, ATTR_LAUNCHER)
    # The child's argv is CHILD and "exec": two offsets and a zero, then the
    # strings.
    p.push_str("exec")
    p.push_str(child)
    p.pushw(0)
    p.pushw(6 + len(child) + 1)
    p.pushw(6)
    p.call(OP_EXEC)
    exit_with(p, 1)
    return p


def waiter_prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc)
    p.symbol("main")
    p.say("waiting\n")
    p.symbol("forever")
    p.bra("forever")
    return p


def child_prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc)
    p.symbol("main")

    # The second offset in ARGV's table is 0 when there is no argv[1].
    p.call(OP_ARGV)
    p.lda_abs(XSTACK)
    p.lda_abs(XSTACK)
    p.lda_abs(XSTACK)
    p.ora_abs(XSTACK)
    p.bne("again")
    p.say("child\n")
    exit_with(p, CHILD_EXIT)

    p.symbol("again")
    p.call(OP_DROP_XSTACK)
    p.say("child exec\n")
    p.push_str(CHILD)
    p.pushw(0)
    p.pushw(4)
    p.call(OP_EXEC)
    exit_with(p, 1)
    return p


CHAIN_SCRIPT = r"""
# The launcher prints its argv[0] each time it starts. The first time, it runs
# the child, which runs itself once more and exits 7; then the launcher is back.

wait "launcher "
wait "/launcher.rp6502\nchild exec\nchild\nlauncher "
wait "/launcher.rp6502\nback 0007\n"
wait exit
expect exit 0
run 10
expect-not "launcher"

# Started by its installed name, it keeps that name when it comes back.
load ":launcher.rp6502"
wait "launcher :launcher.rp6502\nchild exec\nchild\nlauncher :launcher.rp6502\nback 0007\n"
wait exit
expect exit 0
run 10
expect-not "launcher"
"""


def test_chain():
    script(CHAIN_SCRIPT, "--install", rom("launcher"),
           "--install", rom("child"), rom("launcher"))


QUIT_SCRIPT = r"""
# Alt-F4 in a launcher's child exits it as exit(-1), and the launcher is back
# with the code, 65535, not the 7 of the ROM that exited before. In the
# launcher itself, Alt-F4 is an ordinary key.

wait exit
expect exit 7
load ":waitlaunch.rp6502"
wait "waiting\n"
key f4+alt
wait "back FFFF\n"
wait exit
expect exit 0
"""


def test_quit():
    script(QUIT_SCRIPT, "--install", rom("waiter"), "--install",
           rom("waitlaunch"), rom("child"))


BREAK_SCRIPT = r"""
# A break ends the launcher chain: the launcher does not come back.

wait "waiting\n"
sys break
run 10
expect-not "back"
"""


def test_break():
    script(BREAK_SCRIPT, "--install", rom("waiter"), rom("waitlaunch"))


ROMS = {"launcher": launcher_prog, "child": child_prog, "waiter": waiter_prog,
        "waitlaunch": lambda: launcher_prog(WAITER)}
TESTS = {"chain": test_chain, "chain_quit": test_quit, "chain_break": test_break}
main(globals())
