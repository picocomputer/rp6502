#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The program draws one lrand, keeps it in RAM, prints a line and exits with
# EXIT_CODE. A driver checks the draw against the seed that the script set.

import os
import shutil
import subprocess
from pathlib import Path

import rp6502
from rp6502_asm import (API_A, API_X, OP_ATTR_GET, OP_EXIT, Asm, putc)
from rp6502_rom import main, rom

ATTR_LRAND = 0x04
EXIT_CODE = 3
GOT = 0x0210


def lrand(seed):
    """The draw that sys_random gives first after a seed, masked to 31 bits as
    the lrand attribute masks it."""
    m = 0xFFFFFFFF
    x = (seed * 1664525 + 1013904223) & m
    x ^= x >> 16
    x = (x * 0x7FEB352D) & m
    x ^= x >> 15
    x = (x * 0x846CA68B) & m
    x ^= x >> 16
    return x & 0x7FFFFFFF


def prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc)
    p.symbol("main")
    # The 32-bit answer is in A, X and SREG at $FFF4, $FFF6 and $FFF8.
    p.call_a(OP_ATTR_GET, ATTR_LRAND)
    for i, reg in enumerate((0xFFF4, 0xFFF6, 0xFFF8, 0xFFF9)):
        p.lda_abs(reg)
        p.sta_abs(GOT + i)
    p.say("drew\n")
    p.store(API_A, EXIT_CODE)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()
    return p


def quoted(path):
    """A host path as a script string. A script reads a backslash as an escape,
    so the string has forward slashes, which Windows also takes."""
    return '"' + Path(path).as_posix() + '"'


def drew(e):
    return int.from_bytes(bytes.fromhex(e.cmd("dump GOT 4")), "little")


def test_seed():
    """seed sets the stream that the program draws from, before its first
    frame and before a load, and status shows it."""
    with rp6502.Emu(rom("seed")) as e:
        e.cmd("seed 5")
        assert " SEED=5 " in e.cmd("status"), e.cmd("status")
        e.cmd("wait exit")
        e.cmd(f"expect exit {EXIT_CODE}")
        e.cmd("expect-not exit 0")
        assert drew(e) == lrand(5), f"{drew(e):08X} is not lrand(5)"
        # A seed can be any expression, such as one that a loop builds.
        for seed, value in ((0, 0), (4294967295, 4294967295), ("(2 + 3)", 5)):
            e.cmd("discard")
            e.cmd(f"seed {seed}")
            e.cmd(f'load {quoted(rom("seed"))}')
            e.cmd("wait exit")
            e.cmd('expect "drew\\n"')
            assert drew(e) == lrand(value), f"seed {seed} drew {drew(e):08X}"


def fails(lines, want, *args):
    """Sends lines to an emulator of its own, because a failure ends the run,
    and checks that the last line fails with want. Returns the status line
    that follows the failure."""
    r = subprocess.run(rp6502.Emu.command("--script", "-", *args, rom("seed")),
                       input="\n".join(lines) + "\n",
                       capture_output=True, text=True)
    out = [x for x in r.stderr.splitlines() if x.startswith("rp6502-emu: ")]
    assert (r.returncode == 1 and len(out) == 2 and
            out[0] == f"rp6502-emu: <stdin>:{len(lines)}: {want}" and
            out[1].startswith("rp6502-emu: PC=")), \
        f"{lines[-1]!r} should fail with {want!r}, but the emulator exited " \
        f"{r.returncode} with {r.stderr!r}"
    return out[1]


def test_seed_range():
    """A seed is 0 to 4294967295 on every system."""
    for seed in ("4294967296", "-1", ""):
        fails([f"seed {seed}"], "seed wants a number from 0 to 4294967295")


def test_leftover():
    """Words after the last argument fail, so an expression with spaces is
    not cut short without a word."""
    fails(["run 1 + 1"], "unexpected '+'; spaces separate arguments")
    fails(["seed 2 + 3"], "unexpected '+'; spaces separate arguments")


def test_failure_line():
    """A failure ends with the cpu, the exit code and both seeds."""
    line = fails(["seed 5", "wait exit", "expect exit 0"],
                 f"exit code {EXIT_CODE}, expected 0", "--memseed", "7")
    want = f" RESB=0 EXIT={EXIT_CODE} SEED=5 MEMSEED=7 ROM="
    assert want in line and line.endswith("/seed.rp6502"), line
    # Emu gives the same fields with the error.
    try:
        with rp6502.Emu("--memseed", "7", rom("seed")) as e:
            e.cmd("wait exit")
            e.cmd("expect exit 0")
    except rp6502.ScriptError as err:
        assert err.status["EXIT"] == EXIT_CODE, err.status
        assert err.status["ROM"].endswith("/seed.rp6502"), err.status
    else:
        raise AssertionError("expect exit 0 passed")


def test_memfill():
    """status names the fill that took effect: MEMFILL for one byte, else
    MEMSEED."""
    with rp6502.Emu("--memfill", "$55", rom("seed")) as e:
        s = e.status()
        assert s["MEMFILL"] == 0x55 and "MEMSEED" not in s, s


def test_status():
    """status names the running ROM last, so a path with spaces needs no
    quotes, and Emu.status gives the fields as a dict."""
    spaced = os.path.join(os.getcwd(), "a b")
    os.makedirs(spaced, exist_ok=True)
    shutil.copy(rom("seed"), os.path.join(spaced, "my rom.rp6502"))
    with rp6502.Emu(rom("seed")) as e:
        e.cmd("seed 9")
        s = e.status()
        assert s["SEED"] == 9 and s["RESB"] == 1 and "EXIT" not in s, s
        assert s["ROM"].endswith("/seed.rp6502"), s
        e.cmd("wait exit")
        s = e.status()
        assert s["EXIT"] == EXIT_CODE and s["RESB"] == 0, s
        e.cmd(f'load {quoted(os.path.join(spaced, "my rom.rp6502"))}')
        s = e.status()
        assert s["ROM"].endswith("/a b/my rom.rp6502"), s
        assert e.cmd("status").endswith(" ROM=" + s["ROM"])


def test_exit_checks():
    """expect exit checks without running frames, so it fails while the
    program runs. A stop that the script asks for is not an exit, even after
    another program exited."""
    fails(["expect exit 0"], "the program has not exited")
    fails(["sys stop", "expect exit 0"], "the program has not exited")
    fails(["wait exit", f'load {quoted(rom("seed"))}', "sys stop", "expect exit 3"],
          "the program has not exited")
    with rp6502.Emu(rom("seed")) as e:
        e.cmd("wait exit")
        e.cmd("sys stop")
        e.cmd(f"expect exit {EXIT_CODE}")
    line = fails(["sys stop", "status", "wait exit 5"],
                 "timed out waiting for the program to exit")
    assert " EXIT=" not in line, line


def test_load_errors():
    """A ROM that does not start gives the same message as the monitor of an
    RP6502-PICO, on the command line and from load."""
    novec = rp6502.ROM()
    novec.add_binary_data(b"\xea", 0x0200)
    novec.write("novec.rp6502")
    assets = rp6502.ROM()
    assets.add_asset("help", b"help")
    assets.write("assets.rp6502")
    r = subprocess.run(rp6502.Emu.command("--headless", "novec.rp6502"),
                       capture_output=True, text=True)
    assert (r.returncode, r.stderr) == \
        (1, "rp6502-emu: ?ROM has no reset vector\n"), r
    for name, want in (("novec.rp6502", "?ROM has no reset vector"),
                       ("assets.rp6502", "?ROM data invalid"),
                       ("missing.rp6502", "?No file")):
        fails(["wait exit", f"load {quoted(os.path.abspath(name))}"], want)


def test_discard():
    """discard drops the output that no check took, one stream or all."""
    with rp6502.Emu(rom("seed")) as e:
        e.cmd("wait exit")
        e.cmd("discard stdout")
        e.cmd('expect-not "drew"')
        e.cmd('expect console "drew"')
    fails(["discard stdin"], "discard wants stdout, stderr or console")


ROMS = {"seed": prog}
TESTS = {"seed": test_seed, "seed_range": test_seed_range,
         "leftover": test_leftover, "failure_line": test_failure_line,
         "exit_checks": test_exit_checks, "discard": test_discard,
         "memfill": test_memfill, "status": test_status,
         "load_errors": test_load_errors}
main(globals())
