#!/usr/bin/env python3

import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def units(compdb, srcdir):
    with open(compdb) as f:
        db = json.load(f)
    srcdir = os.path.normpath(srcdir)
    out = []
    for entry in db:
        if "/emu_core.dir/" not in entry.get("output", ""):
            continue
        src = os.path.normpath(entry["file"])
        if os.path.commonpath([src, srcdir]) != srcdir:
            continue
        out.append((src, shlex.split(entry["command"])))
    if not out:
        raise SystemExit("no emu_core units in %s: nothing was checked" % compdb)
    return sorted(out)


def objects_in(src, cmd, tmp):
    """The unit is rebuilt without link-time optimization because nm lists
    no file-scope static in an LTO object."""
    obj = os.path.join(tmp, re.sub(r"\W", "_", src) + ".o")
    keep = []
    skip_next = False
    for arg in cmd[1:]:
        if skip_next:
            skip_next = False
            continue
        if arg in ("-o", "-c"):
            skip_next = arg == "-o"
            continue
        if arg.startswith("-flto") or arg == "-fno-fat-lto-objects":
            continue
        keep.append(arg)
    run = [cmd[0]] + keep + ["-c", "-o", obj]
    r = subprocess.run(run, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("cannot rebuild %s:\n%s" % (src, r.stderr))
    r = subprocess.run(["nm", "--defined-only", obj], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("nm failed on %s:\n%s" % (obj, r.stderr))
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        _, kind, name = parts
        # nm marks .bss symbols b or B, .data symbols d or D, and initialized
        # small-data symbols g or G.
        if kind not in "bBdDgG":
            continue
        if "MergedGlobals" in name:
            raise SystemExit(
                "%s: this toolchain merges file-scope objects into %s, so they "
                "cannot be counted one by one" % (src, name))
        yield name
    os.unlink(obj)


def read_list(path):
    want = {}
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            if ":" not in line:
                raise SystemExit("%s:%d: want '<file> <symbol>: <why>'" % (path, n))
            what, why = line.split(":", 1)
            bits = what.split()
            if len(bits) != 2 or not why.strip():
                raise SystemExit("%s:%d: want '<file> <symbol>: <why>'" % (path, n))
            want[(bits[0], bits[1])] = why.strip()
    return want


def main():
    compdb, srcdir, listing = sys.argv[1], os.path.normpath(sys.argv[2]), sys.argv[3]
    bless = "--bless" in sys.argv[4:]
    found = set()
    with tempfile.TemporaryDirectory() as tmp:
        for src, cmd in units(compdb, srcdir):
            rel = os.path.relpath(src, srcdir)
            for name in objects_in(src, cmd, tmp):
                found.add((rel, name))

    if bless:
        for rel, name in sorted(found):
            print("%s %s: " % (rel, name))
        return 0

    want = read_list(listing)
    new = sorted(found - set(want))
    gone = sorted(set(want) - found)
    if new:
        print("state the machine owns and nothing has written down:\n")
        for rel, name in new:
            print("  %s %s" % (rel, name))
        print("\nEach one is machine state a savestate does not carry. Give it")
        print("a row's chunk, or a line in %s" % os.path.relpath(listing, os.getcwd()))
        print("saying which chunk carries it, or why nothing has to.")
    if gone:
        print("\nwritten down but no longer there:\n")
        for rel, name in gone:
            print("  %s %s" % (rel, name))
        print("\nRemove those lines from %s." % os.path.relpath(listing, os.getcwd()))
    return 1 if (new or gone) else 0


if __name__ == "__main__":
    sys.exit(main())
