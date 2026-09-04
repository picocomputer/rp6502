#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# tools/rp6502.py against itself: what its writer emits, its reader has to
# take back. That was not true until the generators started packaging
# through it — they wrote a headerless form the reader rejected outright,
# so a generated ROM could never be merged into another or sent with
# `rp6502.py run`, and nobody found out because nothing tried.
#
# The chunking is checked here too, because it is the half of the format
# that decides whether an image loads on hardware at all. The monitor
# reads a chunk into one buffer and refuses a longer one, so a writer that
# emits a program as a single record makes a ROM the emulator and the
# simulation both run and a Pico will not.

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# The generators load the tool the same way and for the same reasons, so
# the loader is theirs rather than written twice.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "gen"))
from rp6502_rom import tool  # noqa: E402

# The monitor's buffer, src/host/pico/ria/sys/mbuf.h.
MBUF_SIZE = 1024


def check(scratch):
    t = tool()
    fails = []

    def fail(msg):
        fails.append(msg)
        print(f"rom_tool: {msg}", file=sys.stderr)

    # A program big enough to need chunking, an XRAM block to exercise the
    # wider address, a named asset, and the reset vector.
    prog = bytes((i * 13 + 7) & 0xFF for i in range(3000))
    xram = bytes((i * 7 + 3) & 0xFF for i in range(600))
    rom = t.ROM()
    rom.add_binary_data(prog, 0x0300)
    rom.add_binary_data(xram, 0x10000)
    rom.add_asset("help", b"a named asset\n")
    rom.add_reset_vector(0x0300)

    path = Path(scratch) / "rom_tool_check.rp6502"
    rom.write(path)

    # Read it back with the reader that used to refuse what the generators
    # wrote, and compare the memory image rather than the file: the writer
    # is free to chunk differently, the bytes it stands for are not.
    back = t.ROM()
    back.add_rom_file(str(path))
    if back.data != rom.data:
        fail("the reader did not reproduce the writer's memory image")
    if back.assets != rom.assets:
        fail(f"named assets did not survive: {back.assets}")
    if not back.has_reset_vector():
        fail("the reset vector did not survive")

    # Merging is the other half of what the reader is for.
    merged = t.ROM()
    merged.add_rom_file(str(path))
    merged_path = Path(scratch) / "rom_tool_merged.rp6502"
    merged.write(merged_path)
    again = t.ROM()
    again.add_rom_file(str(merged_path))
    if again.data != rom.data:
        fail("a merged image did not reproduce the original")

    # Every chunk within the monitor's buffer, and none crossing a page.
    addr, data = rom.next_rom_data(0)
    while data is not None:
        if len(data) > MBUF_SIZE:
            fail(f"chunk at ${addr:05X} is {len(data)} bytes, over MBUF_SIZE")
        if (addr >> 16) != ((addr + len(data) - 1) >> 16):
            fail(f"chunk at ${addr:05X} crosses a 64K boundary")
        addr += len(data)
        addr, data = rom.next_rom_data(addr)

    # An overlap is a program quietly overwriting its own assets, which is
    # what the headerless writer allowed and what this one refuses.
    dup = t.ROM()
    dup.add_binary_data(b"\1\2\3\4", 0x0300)
    try:
        dup.add_binary_data(b"\5\6", 0x0301)
    except t.ROMException:
        pass
    else:
        fail("overlapping records were accepted")

    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scratch", required=True,
                    help="directory to write throwaway images into")
    a = ap.parse_args()
    fails = check(a.scratch)
    if fails:
        return 1
    print("rom_tool: writer and reader agree, chunks fit MBUF_SIZE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
