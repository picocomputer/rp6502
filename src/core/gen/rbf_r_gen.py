# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The Pocket loads its bitstream with each byte's bits reversed:
# the packaged bitstream is the Quartus .rbf with every byte
# bit-flipped, nothing more. Verified against the official core
# template's shipped pair. Reversal is an involution, so running this
# twice is identity.
#
# core.json names the file, and this core calls it core.bin. The
# conventional name is bitstream.rbf_r; the loader reads whatever the
# manifest says, and the reversal is about the bytes rather than the
# name.

import sys

REV = bytes(int(f"{b:08b}"[::-1], 2) for b in range(256))


def reverse(data: bytes) -> bytes:
    return data.translate(REV)


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "--check":
        assert reverse(bytes([0x01, 0x80, 0xFF, 0x00, 0xA5])) == bytes(
            [0x80, 0x01, 0xFF, 0x00, 0xA5])
        probe = bytes(range(256)) * 7
        assert reverse(reverse(probe)) == probe
        return 0
    if len(sys.argv) != 3:
        print("usage: rbf_r_gen.py <in.rbf> <out.bin>", file=sys.stderr)
        return 2
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    with open(sys.argv[2], "wb") as f:
        f.write(reverse(data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
