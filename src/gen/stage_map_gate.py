#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The staging store's map lives in three files that cannot reference
# each other. data.json tells the host where to write each slot; mmio.h
# tells the firmware where to read it; tb_stage.h tells the bench where
# to play the host. They are the same map, and strict JSON cannot carry
# the comment that says so.
#
# A disagreement here does not fail to build and does not fail loudly:
# the host writes an image where the firmware is not looking, and the
# machine comes up with a blank screen or a ROM that will not parse. A
# bench that drifts is worse, because it agrees with itself and passes.
# So the map is checked rather than maintained by hand.

import argparse
import json
import re
from pathlib import Path

# The window the soft CPU reads the staging store through. Bridge
# addresses are what the host is given; these are the same bytes.
STAGE_BASE = 0x60000000

# Slot ids that are windows for open files, in descriptor order.
FILE_SLOT_FIRST = 1
FILE_SLOT_COUNT = 8

# The named assets, by slot id and by the pointer mmio.h gives them.
ASSET_SLOTS = {9: "FONTS", 10: "OEMCP", 11: "KBDLAY"}


def defines(path: Path) -> dict:
    """Every #define in mmio.h whose body holds one integer literal."""
    out = {}
    pat = re.compile(r"^#define\s+(\w+)\b(.*)$")
    num = re.compile(r"0[xX][0-9a-fA-F]+|\b\d+\b")
    for line in path.read_text(encoding="utf-8").splitlines():
        m = pat.match(line)
        if not m:
            continue
        found = num.findall(m.group(2))
        if len(found) == 1:
            out[m.group(1)] = int(found[0], 0)
    return out


def slots(path: Path) -> dict:
    doc = json.loads(path.read_text(encoding="utf-8"))
    return {int(s["id"]): s for s in doc["data"]["data_slots"]}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--mmio", required=True)
    ap.add_argument("--bench")
    a = ap.parse_args()

    ds = slots(Path(a.data))
    mm = defines(Path(a.mmio))

    bad = []

    def want(what, got, expect):
        if got != expect:
            bad.append(f"{what}: {got:#010x} is not {expect:#010x}")

    def addr(slot_id):
        return int(ds[slot_id]["address"], 0)

    # The ROM owns everything below its ceiling, and the ceiling is what
    # msc_stage_rom refuses to exceed.
    want("slot 0 address", addr(0), mm["ROM_BRIDGE"])
    want("slot 0 size_maximum", int(ds[0]["size_maximum"], 0), mm["ROM_MAX"])

    # The blob is the gap between the ROM's ceiling and the first file
    # window. Both edges, so neither can move alone.
    want("savestate blob base", mm["ROM_MAX"], mm["SST_BLOB_BRIDGE"])
    want("savestate blob ceiling",
         mm["SST_BLOB_BRIDGE"] + mm["SST_BLOB_MAX"],
         addr(FILE_SLOT_FIRST))
    want("savestate blob window",
         mm["SST_BLOB"], STAGE_BASE + mm["SST_BLOB_BRIDGE"])

    # Descriptor d is slot 1 + d, and its window is the d'th above the
    # blob. The stride is one constant on both sides.
    for d in range(FILE_SLOT_COUNT):
        base = addr(FILE_SLOT_FIRST) + d * mm["SLOT_WIN_SIZE"]
        want(f"file slot {FILE_SLOT_FIRST + d} address",
             addr(FILE_SLOT_FIRST + d), base)

    for slot_id, name in ASSET_SLOTS.items():
        want(f"slot {slot_id} ({ds[slot_id]['name']}) address",
             addr(slot_id), mm[name] - STAGE_BASE)

    # Get File's answer lands in a scratch that is not a slot, so
    # nothing but this check keeps a slot from being grown over it.
    want("Get File scratch window",
         mm["GETFILE_WIN"], STAGE_BASE + mm["GETFILE_BRIDGE"])

    # Ascending, and no slot overlapping the next. The asset slots
    # declare an exact size; the file windows take the stride.
    climb = sorted(ds)
    for lo, hi in zip(climb, climb[1:]):
        span = (int(ds[lo]["size_exact"], 0) if "size_exact" in ds[lo]
                else mm["SLOT_WIN_SIZE"])
        if lo == 0:
            span = mm["ROM_MAX"] + mm["SST_BLOB_MAX"]
        if addr(lo) + span > addr(hi):
            bad.append(f"slot {lo} runs {addr(lo) + span:#010x} into "
                       f"slot {hi} at {addr(hi):#010x}")

    scratch = mm["GETFILE_BRIDGE"]
    for slot_id, s in ds.items():
        if "size_exact" not in s:
            continue
        end = addr(slot_id) + int(s["size_exact"], 0)
        if addr(slot_id) <= scratch < end:
            bad.append(f"slot {slot_id} covers the Get File scratch at "
                       f"{scratch:#010x}")

    # The bench plays the host, so its copy of the map has to be the
    # same map or the tests agree with themselves and prove nothing.
    if a.bench:
        tb = defines(Path(a.bench))
        want("bench ROM base", tb["TB_STAGE_ROM_BASE"], mm["ROM_BRIDGE"])
        want("bench window base",
             tb["TB_STAGE_WIN_BASE"], addr(FILE_SLOT_FIRST))
        want("bench window size",
             tb["TB_STAGE_WIN_SIZE"], mm["SLOT_WIN_SIZE"])
        for slot_id, name in ASSET_SLOTS.items():
            tag = {"FONTS": "FONT", "OEMCP": "OEMCP",
                   "KBDLAY": "KBDLAY"}[name]
            want(f"bench {tag.lower()} base",
                 tb[f"TB_STAGE_{tag}_BASE"], addr(slot_id))
            # The bench reserves a rounded window; it may not be smaller
            # than the file the host will put there.
            exact = int(ds[slot_id]["size_exact"], 0)
            if tb[f"TB_STAGE_{tag}_SIZE"] < exact:
                bad.append(f"bench {tag.lower()} window "
                           f"{tb[f'TB_STAGE_{tag}_SIZE']:#x} is under "
                           f"slot {slot_id}'s {exact:#x}")

    if bad:
        raise SystemExit("stage_map_gate: the staging map disagrees\n"
                         + "\n".join(f"  {line}" for line in bad))

    print(f"stage map ok: {len(ds)} slots, ROM ceiling {mm['ROM_MAX']:#010x}, "
          f"blob {mm['SST_BLOB_MAX'] // 1024} KB at "
          f"{mm['SST_BLOB_BRIDGE']:#010x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
