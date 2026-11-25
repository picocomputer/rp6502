#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""A Python test that reads its own words with rp6502.parse_args() leaves
the emulator the options that --list-options lists, as getopt_long reads
them, and the ROMs and the ROM arguments. An emulator from before
--list-options gets every word, as it did before."""

import argparse
import os
import subprocess
import sys

import rp6502
from rp6502_rom import main


def argv(*words):
    """Gives the test the arguments <words>, which no test has read."""
    sys.argv[1:] = words
    rp6502.Emu._roms = None


def parser(*options):
    """A parser of the test, with each option in <options>, which takes a
    value."""
    p = argparse.ArgumentParser()
    p.add_argument("-v", action="store_true")
    for option in options:
        p.add_argument(option)
    return p


def raises(call, kind):
    """The message of the <kind> that <call> raises, or None."""
    try:
        call()
    except kind as e:
        return str(e)
    return None


def test_options():
    emu = os.environ["RP6502_EMU"]
    errors = []

    def want(got, wanted, what):
        if got != wanted:
            errors.append(f"{what}: {got!r}, not {wanted!r}")

    # Each option is --name, or --name= when it takes a value, and the help
    # tells of each one.
    listed = subprocess.run([emu, "--list-options"], capture_output=True,
                            text=True, check=True).stdout.split()
    usage = subprocess.run([emu, "--help"], capture_output=True,
                           text=True, check=True).stdout
    want("--frames=" in listed and "--mute" in listed, True, "the list")
    for word in listed:
        if not word.startswith("--") or word.rstrip("=") + " " not in usage:
            errors.append(f"{word} is not in --help")

    # The emulator takes its options with their values, the shortened ones
    # too, the words that are not options, and everything after --. The
    # parser takes the rest, the value of --miss with it.
    argv("--save-dir", "s", "--miss", "5", "--fr", "10", "--memseed=3", "-v",
         "a.rp6502", "--script", "--miss", "--", "x", "--miss")
    args = rp6502.parse_args(parser("--miss"))
    want((args.miss, args.v), ("5", True), "parse_args")
    want(rp6502.Emu.command(), [emu, "--save-dir", "s", "--fr", "10",
                                "--memseed=3", "--script", "--miss",
                                "a.rp6502", "--", "x", "--miss"], "command")

    # A word that is not an option goes to the emulator unless the parser
    # takes it.
    argv("--save-dir", "s", "a.rp6502", "b")
    p = parser()
    p.add_argument("name")
    want(rp6502.parse_args(p).name, "a.rp6502", "a positional")
    want(rp6502.Emu.command(), [emu, "--save-dir", "s", "b"], "its command")

    # A word that names no option, or more than one, is the test's.
    argv("--me", "1", "--mutex", "a.rp6502")
    args = rp6502.parse_args(parser("--me", "--mutex"))
    want((args.me, args.mutex), ("1", "a.rp6502"), "no option of the emulator")
    want(rp6502.Emu.command(), [emu], "its command")

    # The parser refuses a word that it does not know.
    argv("--zap", "a.rp6502")
    want(raises(lambda: rp6502.parse_args(parser()), SystemExit), "2",
         "an unknown word")

    # An option of the parser that the emulator would take is refused, by
    # its whole name or by its start.
    for option in ("--frames", "--fra", "--list-options"):
        argv()
        want(raises(lambda: rp6502.parse_args(parser(option)), rp6502.ScriptError),
             f"{option} is reserved for the emulator", option)

    # A shortened option is the same option for the check of Emu() and the
    # arguments of the test.
    argv("--fr", "10")
    want(raises(lambda: rp6502.Emu.command("--frames", "3"), rp6502.ScriptError),
         "--frames is in Emu() and in the arguments of the test", "a repeat")

    # An option of a subparser is the test's too.
    argv("a.rp6502")
    p = argparse.ArgumentParser()
    p.add_subparsers().add_parser("go").add_argument("--frames")
    want(raises(lambda: rp6502.parse_args(p), rp6502.ScriptError),
         "--frames is reserved for the emulator", "a subparser's option")

    # Python, as the emulator, has no --list-options, so it gets every word,
    # and the test gets none.
    os.environ["RP6502_EMU"] = sys.executable
    try:
        argv("--miss", "5", "--aspect", "4:3", "a.rp6502")
        want(rp6502.parse_args(parser("--miss", "--frames")).miss, None,
             "parse_args of an older emulator")
        want(rp6502.Emu.command(), [sys.executable, "--miss", "5", "--aspect",
                                    "4:3", "a.rp6502"],
             "the command of an older emulator")
        argv("--aspect", "4:3", "--seed", "5")
        want(rp6502.Emu.command("a.rp6502"),
             [sys.executable, "a.rp6502", "--aspect", "4:3", "--seed", "5"],
             "an older emulator's options with values")
    finally:
        os.environ["RP6502_EMU"] = emu
    assert not errors, "\n".join(errors)


def test_options_run():
    """CMake gives the test --miss 5 and --memse 1234 before the ROM. The
    emulator starts with --memse 1234, which is --memseed, and without
    --miss 5."""
    p = argparse.ArgumentParser()
    p.add_argument("--miss", type=int, default=0)
    args = rp6502.parse_args(p)
    assert args.miss == 5, args
    with rp6502.Emu() as e:
        memseed = e.status()["MEMSEED"]
    assert memseed == 1234, f"MEMSEED={memseed}"


TESTS = {"options": test_options, "options_run": test_options_run}
main(globals())
