#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "gen"))
from rp6502_rom import tool  # noqa: E402

# MBUF_SIZE in src/host/pico/ria/sys/mbuf.h is the size of mbuf, the buffer
# the monitor reads each record into.
MBUF_SIZE = 1024


def check(scratch):
    t = tool()
    fails = []

    def fail(msg):
        fails.append(msg)
        print(f"rom_tool: {msg}", file=sys.stderr)

    prog = bytes((i * 13 + 7) & 0xFF for i in range(3000))
    xram = bytes((i * 7 + 3) & 0xFF for i in range(600))
    rom = t.ROM()
    rom.add_binary_data(prog, 0x0300)
    rom.add_binary_data(xram, 0x10000)
    rom.add_asset("help", b"a named asset\n")
    rom.add_reset_vector(0x0300)

    path = Path(scratch) / "rom_tool_check.rp6502"
    rom.write(path)

    back = t.ROM()
    back.add_rom_file(str(path))
    if back.data != rom.data:
        fail("the reader did not reproduce the writer's memory image")
    if back.assets != rom.assets:
        fail(f"named assets did not survive: {back.assets}")
    if not back.has_reset_vector():
        fail("the reset vector did not survive")

    merged = t.ROM()
    merged.add_rom_file(str(path))
    merged_path = Path(scratch) / "rom_tool_merged.rp6502"
    merged.write(merged_path)
    again = t.ROM()
    again.add_rom_file(str(merged_path))
    if again.data != rom.data:
        fail("a merged image did not reproduce the original")

    addr, data = rom.next_rom_data(0)
    while data is not None:
        if len(data) > MBUF_SIZE:
            fail(f"chunk at ${addr:05X} is {len(data)} bytes, over MBUF_SIZE")
        if (addr >> 16) != ((addr + len(data) - 1) >> 16):
            fail(f"chunk at ${addr:05X} crosses a 64K boundary")
        addr += len(data)
        addr, data = rom.next_rom_data(addr)

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
