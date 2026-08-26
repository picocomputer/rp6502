#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The emulator's script channel, driven from Python.
#
# src/host/sokol/script.c is the same protocol in C, and the two are one
# design: `--script -` reads commands from stdin, and `reply` turns on one
# answer per command on stdout -- ok, ok <values>, or fail <why>. A command
# that blocks answers when it finishes, not when it parses, so the reply for
# `run 600` arrives six hundred frames later. That is what makes asking a
# question and acting on the answer race-free, and it is why a driver reads
# the channel rather than sleeping.
#
# A generator that assembles a 6502 program can then drive it here as well,
# in the same file, against the same constants -- which is the point. A
# static script and the program it watches are two files that can disagree.

import os
import queue
import subprocess
import sys
import threading

# What every script test runs the emulator with. --mute so a runner opens no
# audio device, --seed and --fill so a program that asks for entropy or reads
# memory it never wrote is asked the same question every run. The twin of this
# list is in rp6502_add_script_test, which runs the static scripts; neither is
# to be changed alone.
EMU_ARGS = ("--mute", "--seed", "1", "--fill", "0")

# Only against a wedged process. Every blocking verb has its own timeout
# inside the emulator and answers `fail timed out ...` on its own, so this
# firing means the machine stopped talking altogether.
REPLY_TIMEOUT = 300


class ScriptError(Exception):
    """A command answered `fail`, or the machine stopped answering."""


class Emu:
    """One emulator, driven over the pipe. Its console is not captured: the
    child's stderr is this process's, so EMU_ECHO puts the terminal where
    ctest --output-on-failure already looks."""

    def __init__(self, emu, rom, args=(), timeout=REPLY_TIMEOUT, env=None):
        self.timeout = timeout
        env = dict(os.environ, EMU_ECHO="1", **(env or {}))
        self.proc = subprocess.Popen(
            [str(emu), *EMU_ARGS, *args, "--script", "-", str(rom)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            env=env, text=True, bufsize=1)
        # A reader thread rather than a select: these suites run on Windows
        # too, where a pipe is not selectable.
        self.replies = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.proc.stdout:
            self.replies.put(line.rstrip("\r\n"))
        self.replies.put(None)  # the machine stopped talking

    def send(self, line):
        """A command with no answer expected. Replies are off until start(),
        so a whole preamble goes out without waiting for anything."""
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def _answer(self, line):
        try:
            reply = self.replies.get(timeout=self.timeout)
        except queue.Empty:
            raise ScriptError(f"no answer to {line!r} in {self.timeout}s")
        if reply is None:
            raise ScriptError(f"the machine stopped before answering {line!r}")
        if reply.startswith("fail"):
            # script_error ends the run, so nothing further can be asked.
            raise ScriptError(f"{line!r}: {reply[5:] or 'failed'}")
        if reply == "ok":
            return ""
        if reply.startswith("ok "):
            return reply[3:]
        raise ScriptError(f"{line!r}: unexpected answer {reply!r}")

    def start(self):
        """Turn the answers on and read the one for this line: the point from
        which the machine is talking back."""
        self.send("reply")
        self._answer("reply")

    def cmd(self, line):
        """One command, one answer. Returns what an `ok <values>` carried,
        empty for a bare ok, and raises on fail."""
        self.send(line)
        return self._answer(line)

    def close(self):
        """End of script. Returns the emulator's exit code, which is 0 unless
        something in the run failed."""
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            return self.proc.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise ScriptError("the emulator did not exit")

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if exc[0] is not None:
            self.proc.kill()
        return False


def drive(emu, rom, body, args=(), env=None):
    """Run body(e) against a started emulator and return an exit status. The
    shape every driver's --drive wants: failures print and become a 1.

    env adds to the machine's environment, for a driver whose claim needs
    the host arranged a particular way."""
    e = Emu(emu, rom, args, env=env)
    try:
        with e:
            e.start()
            body(e)
        code = e.close()
    except ScriptError as err:
        print(f"{sys.argv[0]}: {err}", file=sys.stderr)
        return 1
    if code:
        print(f"{sys.argv[0]}: the emulator exited {code}", file=sys.stderr)
    return code
