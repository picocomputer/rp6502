#!/usr/bin/env python3
#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

"""Every static and global a unit defines must be the first name of a row of
that unit's savestate table: saved, carried by one of its FN rows, or
skipped. Each unit is built again with SST_CHECK and without optimization,
which keeps every such symbol in the object for nm and has SST_TABLE put
"@SST@name@saved rows@other rows@" into it. See core/sys/sst.h. A table
that saves state must also be named by an SST row of the driver list, which
the check reads from core/sys/sst.c's object when that unit is checked.

    sst_inventory.py COMPILE_COMMANDS SRCDIR OUTPUT_DIR_PART...

A unit is checked when its compile command's output path holds one of the
OUTPUT_DIR_PART strings, for example /emu_core.dir/, and its source is under
SRCDIR. SST_UNITS, a comma-separated list of path parts, checks only the
units whose source path holds one of them."""

import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

import build

# nm marks .bss symbols b or B, .data symbols d or D, and small data g or G.
# Read-only data, r or R, is not state.
STATE_KINDS = "bBdDgG"
IDENT = re.compile(r"[A-Za-z_]\w*")
CHECK = re.compile(rb"@SST@(\w+)@([^@]*)@([^@]*)@")


def units(compdb, srcdir, parts):
    with open(compdb) as f:
        db = json.load(f)
    out = []
    for entry in db:
        output = entry.get("output", "").replace("\\", "/")
        if not any(p in output for p in parts):
            continue
        src = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
        if os.path.commonpath([src, srcdir]) != srcdir:
            continue
        only = os.environ.get("SST_UNITS")
        if only and not any(u in src.replace("\\", "/") for u in only.split(",")):
            continue
        if not src.endswith(".c"):
            continue
        cmd = entry.get("arguments") or shlex.split(entry["command"])
        out.append((src, cmd, entry["directory"]))
    if not out:
        raise SystemExit("no units matching %s in %s: nothing was checked"
                         % (", ".join(parts), compdb))
    return sorted(out)


def check_command(cmd, obj):
    """The unit's command with SST_CHECK, no optimization and no LTO."""
    keep = []
    skip_next = False
    for arg in cmd[1:]:
        if skip_next:
            skip_next = False
            continue
        if arg in ("-o", "-c"):
            skip_next = arg == "-o"
            continue
        if (arg.startswith("-flto") or arg == "-fno-fat-lto-objects"
                or re.fullmatch(r"-O\w*", arg)):
            continue
        keep.append(arg)
    extra = ["-O0", "-DSST_CHECK"]
    if "clang" in os.path.basename(cmd[0]):
        # Clang can merge globals into one symbol on some targets.
        extra.append("-mno-global-merge")
    return [cmd[0]] + keep + extra + ["-c", "-o", obj]


def nm_for(compiler):
    """The nm of the compiler's toolchain: riscv64-unknown-elf-nm beside
    riscv64-unknown-elf-gcc, and the host's nm for the host's compiler."""
    base = os.path.basename(compiler)
    m = re.match(r"(.*-)(?:gcc|cc|clang)(?:-[\d.]+)?(?:\.exe)?$", base)
    if m:
        nm = os.path.join(os.path.dirname(compiler), m.group(1) + "nm")
        if os.path.isfile(nm):
            return nm
    return "nm"


def symbols(nm, obj):
    """The state the object defines, by name."""
    names = set()
    for line in build.run(nm, "--defined-only", obj).splitlines():
        parts = line.split()
        if len(parts) != 3 or parts[1] not in STATE_KINDS:
            continue
        name = parts[2]
        # Mach-O gives a C name a leading underscore, and the assembler's own
        # labels, such as ltmp1 at the start of a section, have none.
        if sys.platform == "darwin":
            if not name.startswith("_"):
                continue
            name = name[1:]
        names.add(name)
    return names


def named(nm, obj):
    """The tables an object refers to without defining: in sst.c's, the ones
    the driver list's SST rows name."""
    names = set()
    for line in build.run(nm, "--undefined-only", obj).splitlines():
        name = line.split()[-1]
        if sys.platform == "darwin" and name.startswith("_"):
            name = name[1:]
        if name.endswith("_sst"):
            names.add(name)
    return names


def rows(text):
    """The first name of each row in a check string's part. A row is its
    kind and its lvalue, "U8 via.acr;"."""
    for row in text.decode().split(";"):
        bits = row.replace("{", " ").replace("}", " ").split()
        if len(bits) >= 2:
            m = IDENT.match(bits[1])
            if m:
                yield m.group(0)


def main():
    compdb, srcdir = sys.argv[1], os.path.abspath(sys.argv[2])
    parts = sys.argv[3:]
    problems = []
    checked = 0
    saving = {}  # each table that saves something, by name, and its unit
    driven = None  # the tables the driver list names, from sst.c
    with tempfile.TemporaryDirectory() as tmp:
        for src, cmd, cwd in units(compdb, srcdir, parts):
            rel = os.path.relpath(src, srcdir).replace("\\", "/")
            nm = nm_for(cmd[0])
            obj = os.path.join(tmp, re.sub(r"\W", "_", rel) + ".o")
            try:
                build.run(*check_command(cmd, obj), cwd=cwd)
            except subprocess.CalledProcessError as e:
                raise SystemExit("cannot build %s with SST_CHECK:\n%s"
                                 % (rel, e.stderr))
            state = symbols(nm, obj)
            if rel == "core/sys/sst.c":
                driven = named(nm, obj)
            with open(obj, "rb") as f:
                tables = CHECK.findall(f.read())
            for name, saved, _ in tables:
                if saved:
                    saving[name.decode() + "_sst"] = rel
            # A table is const, but its pointers can put it in writable data.
            listed = set(t[0].decode() + "_sst" for t in tables)
            prefixes = []
            for _, saved, other in tables:
                listed.update(rows(saved))
                listed.update(rows(other))
                prefixes += re.findall(r"SKIP_PREFIX (\w+);", other.decode())
            for name in sorted(state):
                if "MergedGlobals" in name:
                    problems.append("%s: %s merges its objects into one "
                                    "symbol, so they cannot be checked"
                                    % (rel, name))
                elif "." in name:
                    problems.append("%s %s: a static inside a function; "
                                    "move it to file scope and list it"
                                    % (rel, name))
                elif name not in listed and not name.startswith(
                        tuple(prefixes)):
                    problems.append("%s %s: in no row of the unit's "
                                    "SST_TABLE" % (rel, name))
            checked += 1
    # A table that saves something is in the savestate only through an SST
    # row of the driver list, which sst.c walks. A table of SKIP rows needs
    # none. Checked when sst.c is among the units.
    if driven is not None:
        for name, rel in sorted(saving.items()):
            if name not in driven:
                problems.append("%s %s: saves state, but no SST row of the "
                                "driver list names it" % (rel, name))
    if problems:
        print("state the savestate tables do not account for:\n")
        for p in problems:
            print("  " + p)
        print("\nGive each static a row in its unit's SST_TABLE: a saved row, "
              "IN_FN, or SKIP. Give each table that saves state an SST row "
              "in its driver.")
        return 1
    print("%d units, every static and global accounted for" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
