#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The core's icon and the platform's banner. Analogue documents one
# format for both, under "Image Format" in the packaging-a-core page:
# sixteen bits a pixel, monochrome, brightness in the upper eight, the
# whole raster stored rotated a quarter turn counter-clockwise. There is
# no header and no magic, so a file is right only if it is exactly
# width x height x 2 bytes long and every second byte is zero.
#
# --selftest re-encodes the two reference images that ship in the
# core-template submodule and insists on byte equality, which is the
# only real evidence that the rotation goes the way round it should.
#
# Both of those are a bright field with dark marks, so a picture drawn
# on white converts straight -- and then arrives on the Pocket as a cow
# on a black square. Analogue's "the icon color may be inverted in the
# UI" is not a maybe, and it is not only the icon: the platform banner
# comes out the same way. A file whose stored field is bright displays
# dark. Both images want --invert, which is why the shipped art of other
# cores stores a dark field for a white one.
#
# --at places the artwork's centre at a fraction of the width. The
# platform banner is not centred: the stock cores set their device art a
# little left of middle, and 0.37 is where they sit. Plain centring and
# --left both look wrong beside them.
#
# This is run by hand when the artwork changes and its output is
# committed, so PIL is a tool the author needs and not a thing the
# firmware build depends on.

import sys
from pathlib import Path

import numpy as np
from PIL import Image

ICON = (36, 36)
PLATFORM = (521, 165)


def encode(gray: np.ndarray) -> bytes:
    """gray is (height, width) uint8 brightness."""
    rot = np.rot90(gray, 1)
    out = np.zeros((rot.size, 2), dtype=np.uint8)
    out[:, 0] = rot.reshape(-1)
    return out.tobytes()


def decode(data: bytes, width: int, height: int) -> np.ndarray:
    rot = np.frombuffer(data, dtype=np.uint8)[0::2].reshape(width, height)
    return np.rot90(rot, -1)


def fit(src: Image.Image, width: int, height: int, invert: bool = False,
        left: bool = False, at: float | None = None) -> np.ndarray:
    """Whole picture, aspect kept, on the field the marks sit on."""
    gray = src.convert("L")
    if invert:
        gray = Image.eval(gray, lambda v: 255 - v)
    scale = min(width / gray.width, height / gray.height)
    w = max(1, round(gray.width * scale))
    h = max(1, round(gray.height * scale))
    gray = gray.resize((w, h), Image.LANCZOS)
    field = Image.new("L", (width, height), 0 if invert else 255)
    if at is not None:
        x = round(width * at - w / 2)
    elif left:
        x = 0
    else:
        x = (width - w) // 2
    field.paste(gray, (max(0, min(x, width - w)), (height - h) // 2))
    return np.asarray(field, dtype=np.uint8)


def selftest(template: Path) -> int:
    refs = [
        (template / "dist/icon.bin", ICON),
        (template / "dist/platforms/_images/ex_platform.bin", PLATFORM),
    ]
    bad = 0
    for path, (width, height) in refs:
        if not path.exists():
            print(f"selftest: {path} missing", file=sys.stderr)
            return 1
        data = path.read_bytes()
        if len(data) != width * height * 2:
            print(f"selftest: {path} is {len(data)} bytes, "
                  f"expected {width * height * 2}", file=sys.stderr)
            bad += 1
            continue
        again = encode(decode(data, width, height))
        print(f"selftest: {path.name} {width}x{height} "
              f"{'round trips' if again == data else 'DIFFERS'}")
        bad += again != data
    return 1 if bad else 0


def main() -> int:
    args = sys.argv[1:]
    if args and args[0] == "--selftest":
        return selftest(Path(args[1]))
    invert = "--invert" in args
    left = "--left" in args
    at = None
    if "--at" in args:
        i = args.index("--at")
        at = float(args[i + 1])
        del args[i:i + 2]
    args = [a for a in args if a not in ("--invert", "--left")]
    if len(args) != 4:
        print("usage: pocket_image_gen.py [--invert] [--left] [--at FRAC] "
              "<src.png> <out.bin> <w> <h>\n"
              "       pocket_image_gen.py --selftest <core-template dir>",
              file=sys.stderr)
        return 2
    width, height = int(args[2]), int(args[3])
    data = encode(fit(Image.open(args[0]), width, height, invert, left, at))
    Path(args[1]).write_bytes(data)
    print(f"{args[1]}: {width}x{height}, {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
