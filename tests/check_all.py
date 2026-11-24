#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

"""Build every machine and run the host tests, in parallel.

The firmware trees and the host trees build at once, so their slow link
steps overlap, then the host test suites run at once. Each tree's output is
kept and shown only when it fails. Trees that are not configured are skipped
and named. Exits 0 only when everything that ran passed.
"""

import argparse
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (build directory, ninja targets or [] for all, runs ctest)
TREES = [
    ("build", ["rp6502-ria-w", "rp6502-vga"], False),
    ("build/pico2", ["rp6502-ria", "rp6502-vga"], False),
    ("build/sokol/linux/release", [], True),
    ("build/libretro/release", [], True),
]


def run(cmd, cwd):
    start = time.monotonic()
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout, time.monotonic() - start


def summary(out):
    for line in reversed(out.splitlines()):
        if "tests passed" in line or "tests failed" in line:
            return line.strip()
    return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--no-test", action="store_true", help="build only")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4,
                    help="ctest jobs for each suite")
    args = ap.parse_args()

    trees = []
    for path, targets, test in TREES:
        full = os.path.join(ROOT, path)
        if os.path.exists(os.path.join(full, "build.ninja")):
            trees.append((path, full, targets, test))
        else:
            print(f"skip   {path} (not configured)")

    ok = True
    begin = time.monotonic()
    with ThreadPoolExecutor(len(trees)) as pool:
        builds = {path: pool.submit(run, ["ninja", "-C", full] + targets, ROOT)
                  for path, full, targets, _ in trees}
        for path, _, _, _ in trees:
            rc, out, secs = builds[path].result()
            print(f"{'build ' if rc == 0 else 'FAILED'} {path} {secs:.1f}s")
            if rc:
                ok = False
                print(out)
    if ok and not args.no_test:
        tested = [(path, full) for path, full, _, test in trees if test]
        with ThreadPoolExecutor(max(1, len(tested))) as pool:
            runs = {path: pool.submit(run, ["ctest", f"-j{args.j}",
                                            "--output-on-failure"], full)
                    for path, full in tested}
            for path, _ in tested:
                rc, out, secs = runs[path].result()
                print(f"{'test  ' if rc == 0 else 'FAILED'} {path} {secs:.1f}s "
                      f"{summary(out)}")
                if rc:
                    ok = False
                    print(out)
    print(f"{'ok' if ok else 'FAILED'} in {time.monotonic() - begin:.1f}s")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
