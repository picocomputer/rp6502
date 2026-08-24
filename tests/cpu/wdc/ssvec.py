#!/usr/bin/env python3
#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Re-encode SingleStepTests JSON into the flat binary the conformance test
# reads. Run by CMake into the build tree; nothing it produces is committed.
#
# Parsing a gigabyte of JSON per test run would dwarf the tests, and tests/wdc
# has no C JSON parser to link. Both problems go away by encoding once.

import argparse
import json
import os
import struct
import sys

MAGIC = b'SSV1'


def encode(src, out, count):
    names = sorted(n for n in os.listdir(src) if n.endswith('.json'))
    tests = []
    for name in names:
        path = os.path.join(src, name)
        # cb.json and db.json are zero length upstream: WAI and STP cannot be
        # expressed as a self-contained instruction test.
        if os.path.getsize(path) == 0:
            continue
        opcode = int(os.path.splitext(name)[0], 16)
        with open(path) as f:
            for t in json.load(f)[:count]:
                tests.append((opcode, t))

    with open(out, 'wb') as f:
        f.write(MAGIC)
        f.write(struct.pack('<I', len(tests)))
        for opcode, t in tests:
            f.write(struct.pack('<B', opcode))
            for which in ('initial', 'final'):
                s = t[which]
                f.write(struct.pack('<HBBBBB', s['pc'], s['s'], s['a'],
                                    s['x'], s['y'], s['p']))
                f.write(struct.pack('<H', len(s['ram'])))
                for addr, val in s['ram']:
                    f.write(struct.pack('<HB', addr, val))
            f.write(struct.pack('<B', len(t['cycles'])))
            for addr, val, kind in t['cycles']:
                f.write(struct.pack('<HBB', addr, val, 1 if kind == 'read' else 0))
    return len(tests)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--count', type=int, default=100)
    args = ap.parse_args()

    if not os.path.isdir(args.src):
        sys.stderr.write('missing vectors: %s\n' % args.src)
        return 1
    n = encode(args.src, args.out, args.count)
    print('%s: %d tests' % (os.path.basename(args.out), n))
    return 0


if __name__ == '__main__':
    sys.exit(main())
