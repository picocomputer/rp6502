#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import json
import re
from pathlib import Path

# This is STAGE in mmio.h, the soft CPU's address for bridge address 0 of
# the staging store.
STAGE_BASE = 0x60000000

FILE_SLOT_FIRST = 1
FILE_SLOT_COUNT = 8

ASSET_SLOTS = {9: "FONTS", 10: "OEMCP", 11: "KBDLAY"}


def defines(path: Path) -> dict:
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
    ap.add_argument("--engine")
    ap.add_argument("--tcm", help="package holding widths the engine names")
    ap.add_argument("--sst")
    ap.add_argument("--top")
    a = ap.parse_args()

    ds = slots(Path(a.data))
    mm = defines(Path(a.mmio))

    bad = []

    def want(what, got, expect):
        if got != expect:
            bad.append(f"{what}: {got:#010x} is not {expect:#010x}")

    def addr(slot_id):
        return int(ds[slot_id]["address"], 0)

    want("slot 0 address", addr(0), mm["ROM_BRIDGE"])
    want("slot 0 size_maximum", int(ds[0]["size_maximum"], 0), mm["ROM_MAX"])

    want("savestate blob base", mm["ROM_MAX"], mm["SST_BLOB_BRIDGE"])
    want("savestate blob ceiling",
         mm["SST_BLOB_BRIDGE"] + mm["SST_BLOB_MAX"],
         addr(FILE_SLOT_FIRST))
    want("savestate blob window",
         mm["SST_BLOB"], STAGE_BASE + mm["SST_BLOB_BRIDGE"])

    for d in range(FILE_SLOT_COUNT):
        base = addr(FILE_SLOT_FIRST) + d * mm["SLOT_WIN_SIZE"]
        want(f"file slot {FILE_SLOT_FIRST + d} address",
             addr(FILE_SLOT_FIRST + d), base)

    for slot_id, name in ASSET_SLOTS.items():
        want(f"slot {slot_id} ({ds[slot_id]['name']}) address",
             addr(slot_id), mm[name] - STAGE_BASE)

    want("Get File scratch window",
         mm["GETFILE_WIN"], STAGE_BASE + mm["GETFILE_BRIDGE"])

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
            exact = int(ds[slot_id]["size_exact"], 0)
            if tb[f"TB_STAGE_{tag}_SIZE"] < exact:
                bad.append(f"bench {tag.lower()} window "
                           f"{tb[f'TB_STAGE_{tag}_SIZE']:#x} is under "
                           f"slot {slot_id}'s {exact:#x}")

    words = None
    if a.engine:
        src = Path(a.engine).read_text(encoding="utf-8")
        parts = dict(re.findall(r"localparam int (W_\w+) = (\w+);", src))
        if a.tcm:
            pkg = dict(re.findall(r"localparam int (\w+) = (\d+);",
                                  Path(a.tcm).read_text(encoding="utf-8")))
            parts = {k: pkg.get(v, v) for k, v in parts.items()}
        parts = {k: v for k, v in parts.items() if v.isdigit()}
        order = ["W_HDR", "W_STATE", "W_REGS", "W_SRAM", "W_XRAM",
                 "W_CELLS", "W_XPROG", "W_TCM", "W_END"]
        missing = [k for k in order if k not in parts]
        if missing:
            bad.append(f"sst_engine is missing {', '.join(missing)}")
        else:
            words = sum(int(parts[k]) for k in order)
    if words is not None and a.sst:
        src = Path(a.sst).read_text(encoding="utf-8")
        m = re.search(r"parameter int BLOB_WORDS = (\d+)", src)
        if not m:
            bad.append("pocket_sst has no BLOB_WORDS")
        else:
            want("bridge blob words", int(m.group(1)), words)
    if words is not None and a.top:
        src = Path(a.top).read_text(encoding="utf-8")
        m = re.search(r"savestate_size = 32'd(\d+)", src)
        m2 = re.search(r"savestate_maxloadsize = 32'd(\d+)", src)
        if not m or not m2:
            bad.append("core_top has no savestate size")
        else:
            want("host blob size", int(m.group(1)), words * 4)
            # The host writes a saved state back with the Pocket OS's header
            # in front of the blob and a thumbnail after it, so the maximum
            # load size is the whole blob window and sst_engine searches the
            # first 1024 word offsets of that window for the start of the
            # blob.
            want("host max load size", int(m2.group(1)),
                 mm["SST_BLOB_MAX"])
        if words * 4 > mm["SST_BLOB_MAX"]:
            bad.append(f"blob {words * 4:#x} is over the window's "
                       f"{mm['SST_BLOB_MAX']:#x}")
        m3 = re.search(r"savestate_addr = 32'h([0-9A-Fa-f_]+)", src)
        if m3:
            want("host blob address",
                 int(m3.group(1).replace("_", ""), 16), mm["SST_BLOB_BRIDGE"])

    if bad:
        raise SystemExit("stage_map_gate: the staging map disagrees\n"
                         + "\n".join(f"  {line}" for line in bad))

    blob = f", state {words * 4 // 1024} KB" if words else ""
    print(f"stage map ok: {len(ds)} slots, ROM ceiling {mm['ROM_MAX']:#010x}, "
          f"blob {mm['SST_BLOB_MAX'] // 1024} KB at "
          f"{mm['SST_BLOB_BRIDGE']:#010x}{blob}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
