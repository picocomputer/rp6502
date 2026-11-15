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

FIRMWARE_FILE = "firmware.bin"

# The Pocket's SDRAM is 64 MB. The ROM slot is its first 62 MB, a limit of
# the system: a whole ROM has to load into it, so nothing takes from it.
# Everything else is in the 2 MB above.
SDRAM_BYTES = 0x04000000
ROM_SLOT_BYTES = 0x03E00000


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
    ap.add_argument("--mem", help="package holding the soft CPU's memory")
    ap.add_argument("--sst")
    ap.add_argument("--top")
    ap.add_argument("--core")
    ap.add_argument("--bridge")
    ap.add_argument("--link")
    a = ap.parse_args()

    ds = slots(Path(a.data))
    mm = defines(Path(a.mmio))

    bad = []

    def want(what, got, expect):
        if got != expect:
            bad.append(f"{what}: {got:#010x} is not {expect:#010x}")

    def addr(slot_id):
        return int(ds[slot_id]["address"], 0)

    want("slot 0 address", addr(0), 0)
    want("slot 0 size_maximum", int(ds[0]["size_maximum"], 0),
         ROM_SLOT_BYTES)
    want("ROM base", mm["ROM_BRIDGE"], addr(0))
    want("ROM ceiling", mm["ROM_MAX"], ROM_SLOT_BYTES)

    # The soft CPU's memory follows the ROM slot and the savestate window
    # follows it. The ROM block is at its start and the RAM block after.
    want("memory base", mm["MEM_BRIDGE"], mm["ROM_MAX"])
    want("memory ceiling", mm["MEM_BRIDGE"] + mm["MEM_MAX"],
         mm["SST_BLOB_BRIDGE"])
    want("RAM block base", mm["MEM_RAM"], mm["MEM_ROM_MAX"])
    if mm["MEM_RAM"] + mm["MEM_RAM_MAX"] > mm["MEM_MAX"]:
        bad.append("the RAM block runs off the soft CPU's memory")
    fw = [i for i, s in ds.items() if s.get("filename") == FIRMWARE_FILE]
    if len(fw) != 1:
        bad.append(f"data.json has {len(fw)} {FIRMWARE_FILE} slots")
    else:
        s = ds[fw[0]]
        want("firmware slot address", addr(fw[0]), mm["MEM_BRIDGE"])
        want("firmware slot size_maximum",
             int(s.get("size_maximum", "0"), 0), mm["MEM_ROM_MAX"])
        if not s.get("required") or s.get("deferload"):
            bad.append("the firmware slot is not loaded before the core "
                       "leaves reset")
    want("savestate blob ceiling",
         mm["SST_BLOB_BRIDGE"] + mm["SST_BLOB_MAX"],
         addr(FILE_SLOT_FIRST))
    want("savestate blob window",
         mm["SST_BLOB"], STAGE_BASE + mm["SST_BLOB_BRIDGE"])

    for d in range(FILE_SLOT_COUNT):
        base = addr(FILE_SLOT_FIRST) + d * mm["SLOT_WIN_SIZE"]
        want(f"file slot {FILE_SLOT_FIRST + d} address",
             addr(FILE_SLOT_FIRST + d), base)

    # The ROM, the file slots and the firmware are every slot there is.
    known = {0, *range(FILE_SLOT_FIRST, FILE_SLOT_FIRST + FILE_SLOT_COUNT),
             *fw}
    for slot_id in sorted(set(ds) - known):
        bad.append(f"slot {slot_id} ({ds[slot_id]['name']}) is not the ROM, "
                   "a file slot or the firmware")

    want("Get File scratch window",
         mm["GETFILE_WIN"], STAGE_BASE + mm["GETFILE_BRIDGE"])

    # Every region of the SDRAM, each slot at the most it can hold. No two
    # may share a byte and none may run off the chip. The Get File scratch
    # is one bridge word here; the path the host writes there ends before
    # the next region begins.
    regions = [("the savestate blob window", mm["SST_BLOB_BRIDGE"],
                mm["SST_BLOB_MAX"]),
               ("the Get File scratch", mm["GETFILE_BRIDGE"], 4),
               ("the ROM block", mm["MEM_BRIDGE"], mm["MEM_ROM_MAX"]),
               ("the RAM block", mm["MEM_BRIDGE"] + mm["MEM_RAM"],
                mm["MEM_RAM_MAX"])]
    for slot_id, s in ds.items():
        if s.get("filename") == FIRMWARE_FILE:
            continue
        if "size_exact" in s:
            span = int(s["size_exact"], 0)
        elif "size_maximum" in s:
            span = int(s["size_maximum"], 0)
        elif FILE_SLOT_FIRST <= slot_id < FILE_SLOT_FIRST + FILE_SLOT_COUNT:
            span = mm["SLOT_WIN_SIZE"]
        else:
            bad.append(f"slot {slot_id} states no size")
            continue
        regions.append((f"slot {slot_id}", addr(slot_id), span))
    regions.sort(key=lambda r: r[1])
    for (lo, lo_at, lo_span), (hi, hi_at, _) in zip(regions, regions[1:]):
        if lo_at + lo_span > hi_at:
            bad.append(f"{lo} runs {lo_at + lo_span:#010x} into {hi} at "
                       f"{hi_at:#010x}")
    last, last_at, last_span = regions[-1]
    if last_at + last_span > SDRAM_BYTES:
        bad.append(f"{last} runs {last_at + last_span:#010x} off the SDRAM")

    if a.bench:
        tb = defines(Path(a.bench))
        want("bench ROM base", tb["TB_STAGE_ROM_BASE"], mm["ROM_BRIDGE"])
        want("bench memory base", tb["TB_STAGE_MEM_BASE"], mm["MEM_BRIDGE"])
        want("bench memory size", tb["TB_STAGE_MEM_SIZE"], mm["MEM_MAX"])
        want("bench ROM block size",
             tb["TB_STAGE_MEM_ROM_SIZE"], mm["MEM_ROM_MAX"])
        want("bench RAM block size",
             tb["TB_STAGE_MEM_RAM_SIZE"], mm["MEM_RAM_MAX"])
        want("bench window base",
             tb["TB_STAGE_WIN_BASE"], addr(FILE_SLOT_FIRST))
        want("bench window size",
             tb["TB_STAGE_WIN_SIZE"], mm["SLOT_WIN_SIZE"])

    def sv_hex(path, pattern, what):
        m = re.search(pattern + r" = 32'h([0-9A-Fa-f_]+)",
                      Path(path).read_text(encoding="utf-8"))
        if not m:
            bad.append(f"{what} is not stated")
            return None
        return int(m.group(1).replace("_", ""), 16)

    def want_sv(path, pattern, what, expect):
        got = sv_hex(path, pattern, what)
        if got is not None:
            want(what, got, expect)

    if a.core:
        want_sv(a.core, r"localparam logic \[31:0\] MEM_BASE",
                "pocket_core memory base", mm["MEM_BRIDGE"])
    if a.bridge:
        want_sv(a.bridge, r"parameter logic \[31:0\] MEM_BASE",
                "pocket_bridge memory base", mm["MEM_BRIDGE"])
        m = re.search(r"parameter int MEM_AW = (\d+)",
                      Path(a.bridge).read_text(encoding="utf-8"))
        if not m:
            bad.append("pocket_bridge has no MEM_AW")
        else:
            want("pocket_bridge memory size", 4 << int(m.group(1)),
                 mm["MEM_MAX"])
    pkg = {}
    if a.mem:
        pkg = dict(re.findall(r"localparam int (\w+) = (\d+);",
                              Path(a.mem).read_text(encoding="utf-8")))
        for name, size in (("MEM_WORDS", "MEM_MAX"),
                           ("ROM_WORDS", "MEM_ROM_MAX"),
                           ("RAM_WORDS", "MEM_RAM_MAX")):
            if name not in pkg:
                bad.append(f"the memory package has no {name}")
            else:
                want(f"package {name}", int(pkg[name]) * 4, mm[size])
        if mm["MEM_MAX"] & (mm["MEM_MAX"] - 1) or \
                mm["MEM_BRIDGE"] % mm["MEM_MAX"]:
            bad.append("the soft CPU's memory is not a power of two at a "
                       "multiple of its size")
    if a.link:
        link = Path(a.link).read_text(encoding="utf-8")
        for name, flags, base, size in (("ROM", "rx", 0, "MEM_ROM_MAX"),
                                        ("RAM", "rw", mm["MEM_RAM"],
                                         "MEM_RAM_MAX")):
            m = re.search(rf"{name} \({flags}\) : ORIGIN = (0x[0-9A-Fa-f]+), "
                          r"LENGTH = (\d+)K", link)
            if not m:
                bad.append(f"link.ld has no {name} region")
            else:
                want(f"linked {name} base", int(m.group(1), 16), base)
                want(f"linked {name} size", int(m.group(2)) * 1024, mm[size])

    words = None
    if a.engine:
        src = Path(a.engine).read_text(encoding="utf-8")
        want_sv(a.engine, r"localparam int A_STAGE_OFF",
                "engine blob address", mm["SST_BLOB_BRIDGE"])
        parts = dict(re.findall(r"localparam int (W_\w+) = (\w+);", src))
        parts = {k: pkg.get(v, v) for k, v in parts.items()}
        parts = {k: v for k, v in parts.items() if v.isdigit()}
        order = ["W_HDR", "W_STATE", "W_REGS", "W_SRAM", "W_XRAM",
                 "W_CELLS", "W_XPROG", "W_RAM", "W_AUD", "W_END"]
        missing = [k for k in order if k not in parts]
        if missing:
            bad.append(f"sst_engine is missing {', '.join(missing)}")
        else:
            words = sum(int(parts[k]) for k in order)
    if a.sst:
        want("blob window alignment",
             mm["SST_BLOB_BRIDGE"] % mm["SST_BLOB_MAX"], 0)
    if words is not None and a.sst:
        src = Path(a.sst).read_text(encoding="utf-8")
        m = re.search(r"parameter int BLOB_WORDS = (\d+)", src)
        if not m:
            bad.append("pocket_sst has no BLOB_WORDS")
        else:
            want("bridge blob words", int(m.group(1)), words)
        want_sv(a.sst, r"parameter logic \[31:0\] BLOB_BASE",
                "bridge blob address", mm["SST_BLOB_BRIDGE"])
        want_sv(a.sst, r"parameter logic \[31:0\] BLOB_WINDOW",
                "bridge blob window", mm["SST_BLOB_MAX"])
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
        want_sv(a.top, "savestate_addr", "host blob address",
                mm["SST_BLOB_BRIDGE"])

    if bad:
        raise SystemExit("stage_map_gate: the staging map disagrees\n"
                         + "\n".join(f"  {line}" for line in bad))

    blob = f", state {words * 4 // 1024} KB" if words else ""
    print(f"stage map ok: {len(ds)} slots, ROM ceiling {mm['ROM_MAX']:#010x}, "
          f"ROM block {mm['MEM_ROM_MAX'] // 1024} KB, "
          f"RAM block {mm['MEM_RAM_MAX'] // 1024} KB, "
          f"blob {mm['SST_BLOB_MAX'] // 1024} KB at "
          f"{mm['SST_BLOB_BRIDGE']:#010x}{blob}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
