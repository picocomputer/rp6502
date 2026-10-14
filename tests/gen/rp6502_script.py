#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The emulator sends the reply to a script command when the command finishes,
# not when it is parsed, so the reply to `run 600` arrives six hundred frames
# later. A driver that reads each reply before it acts therefore has no race
# with the machine and never needs to sleep.

import os
import queue
import subprocess
import sys
import threading

EMU_ARGS = ("--seed", "1", "--fill", "0")

REPLY_TIMEOUT = 300


class ScriptError(Exception):
    """ScriptError is raised when a command fails, or when the emulator
    replies unexpectedly, stops replying or does not exit."""


class Emu:
    def __init__(self, emu, rom, args=(), timeout=REPLY_TIMEOUT, env=None,
                 mute=True, rom_args=()):
        self.timeout = timeout
        env = dict(os.environ, **env) if env else None
        self.proc = subprocess.Popen(
            [str(emu), *(("--mute",) if mute else ()), *EMU_ARGS, *args,
             "--script", "-", str(rom), *(("--", *rom_args) if rom_args else ())],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            env=env, text=True, encoding="utf-8", errors="replace", bufsize=1)
        # The replies are read on a thread rather than with select, because
        # these suites also run on Windows, where select does not accept a
        # pipe.
        self.replies = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.proc.stdout:
            self.replies.put(line.rstrip("\r\n"))
        self.replies.put(None)

    def send(self, line):
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
            raise ScriptError(f"{line!r}: {reply[5:] or 'failed'}{self._rest()}")
        if reply == "ok":
            return ""
        if reply.startswith("ok "):
            return reply[3:]
        raise ScriptError(f"{line!r}: unexpected answer {reply!r}")

    def _rest(self):
        """A fail ends the run, and the rest of stdout is the program's output
        that no check used, up to EOF."""
        lines = []
        while True:
            try:
                out = self.replies.get(timeout=self.timeout)
            except queue.Empty:
                break
            if out is None:
                break
            lines.append(out)
        return "".join("\n" + out for out in lines)

    def start(self):
        self.send("reply")
        self._answer("reply")

    def cmd(self, line):
        self.send(line)
        return self._answer(line)

    def close(self):
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


def drive(emu, rom, body, args=(), env=None, save_dir=None, mute=True,
          rom_args=()):
    if save_dir:
        args = ("--save-dir", str(save_dir), *args)
    e = Emu(emu, rom, args, env=env, mute=mute, rom_args=rom_args)
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
