#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The keyboard layouts, lifted out of src/core/def/keyboard_*.def.
#
# The def files stay the source of truth and stay the thing a
# contributor edits; see def/keyboard.def. What changes is where the numbers
# end up. As X macros they became seven [128][5] DWORD tables plus five
# parallel lookup arrays — twenty kilobytes of flash that a machine with
# no flash cannot have. The Pocket links its firmware into a 96 KB
# tightly coupled memory and reads its bulk data out of a staging store,
# so the layouts come here and become an asset like the fonts and the
# code page tables.
#
# Same numbers, two outputs: a binary for the machine that stages it,
# and a C array for the machine that can afford to link it in.

import argparse
import json
import re
from pathlib import Path

MAGIC = 0x4C4B  # 'KL'

# Buffer sizes a caller declares, NUL included. The record carries the
# bytes without one, so a name that filled its field would come back
# unterminated; the generator refuses that instead.
NAME_MAX = 8
DESC_MAX = 32

NAME_WORDS = NAME_MAX // 2
DESC_WORDS = DESC_MAX // 2
CAPS_WORDS = 8       # 128 keycodes, one bit each
KEY_WORDS = 128 * 4  # keycode * 4 + column

OFF_NAME = 0
OFF_DESC = OFF_NAME + NAME_WORDS
OFF_CAPS = OFF_DESC + DESC_WORDS
OFF_D2N = OFF_CAPS + CAPS_WORDS
OFF_D3N = OFF_D2N + 1
OFF_KEYS = OFF_D3N + 1
OFF_DEAD = OFF_KEYS + KEY_WORDS

# keyboard.c caches the dead key tables as OEM characters in a fixed buffer.
# Its overflow path throws the layout's dead keys away, which is a
# silent loss of function; catching it here means it can only happen to
# a layout nobody has built yet.
DEADKEY_CACHE_SIZE = 512

ESCAPES = {"n": 0x0A, "r": 0x0D, "t": 0x09, "b": 0x08, "f": 0x0C,
           "v": 0x0B, "a": 0x07, "0": 0x00, "\\": 0x5C, "'": 0x27,
           '"': 0x22, "?": 0x3F}


def strip_comments(src):
    """Comments out, string and character literals intact."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c in "'\"":
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == c:
                    j += 1
                    break
                j += 1
            out.append(src[i:j])
            i = j
        elif src.startswith("//", i):
            while i < n and src[i] != "\n":
                i += 1
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def split_args(text):
    """Top level commas only; a literal may hold one, and '(' and ')'."""
    args, depth, start = [], 0, 0
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in "'\"":
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == c:
                    break
                i += 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == "," and depth == 0:
            args.append(text[start:i].strip())
            start = i + 1
        i += 1
    args.append(text[start:].strip())
    return args


def find_calls(src, name):
    """Every `name(...)` invocation, as its argument list."""
    calls = []
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", src):
        i, depth, n = m.end(), 1, len(src)
        while i < n and depth:
            c = src[i]
            if c in "'\"":
                i += 1
                while i < n:
                    if src[i] == "\\":
                        i += 2
                        continue
                    if src[i] == c:
                        break
                    i += 1
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if not depth:
                    break
            i += 1
        calls.append(split_args(src[m.end():i]))
    return calls


def eval_char(lit, where):
    body = lit[1:-1]
    if not body:
        raise SystemExit(f"keyboard_layout_gen: {where}: empty character")
    if body[0] != "\\":
        if len(body) != 1:
            raise SystemExit(f"keyboard_layout_gen: {where}: multi-char {lit}")
        return ord(body)
    esc = body[1]
    if esc == "x":
        return int(body[2:], 16)
    if esc in ESCAPES and len(body) == 2:
        return ESCAPES[esc]
    if esc.isdigit():
        return int(body[1:], 8)
    raise SystemExit(f"keyboard_layout_gen: {where}: bad escape {lit}")


def eval_value(arg, where):
    if arg == "true":
        return 1
    if arg == "false":
        return 0
    if arg.startswith("'"):
        return eval_char(arg, where)
    try:
        return int(arg, 0)
    except ValueError:
        raise SystemExit(f"keyboard_layout_gen: {where}: cannot read {arg}")


def eval_string(arg, where):
    if len(arg) < 2 or arg[0] != '"' or arg[-1] != '"':
        raise SystemExit(f"keyboard_layout_gen: {where}: expected a string, got {arg}")
    return arg[1:-1].encode("ascii").decode("unicode_escape")


def parse_layout(path):
    src = strip_comments(Path(path).read_text(encoding="utf-8"))
    where = Path(path).name

    begin = find_calls(src, "XBEGIN")
    if len(begin) != 1:
        raise SystemExit(f"keyboard_layout_gen: {where}: expected one XBEGIN")
    name = eval_string(begin[0][0], where)
    desc = eval_string(begin[0][1], where)
    if len(name.encode()) >= NAME_MAX:
        raise SystemExit(f"keyboard_layout_gen: {where}: name {name!r} over {NAME_MAX - 1}")
    if len(desc.encode()) >= DESC_MAX:
        raise SystemExit(f"keyboard_layout_gen: {where}: description over {DESC_MAX - 1}")

    keys = [[0] * 4 for _ in range(128)]
    caps = [False] * 128
    for args in find_calls(src, "XKEY"):
        if len(args) != 6:
            raise SystemExit(f"keyboard_layout_gen: {where}: XKEY takes six fields")
        kc = eval_value(args[0], where)
        if not 0 <= kc < 128:
            raise SystemExit(f"keyboard_layout_gen: {where}: keycode {kc:#x} over 0x7F")
        for col in range(4):
            cp = eval_value(args[1 + col], where)
            if cp > 0xFFFF:
                raise SystemExit(f"keyboard_layout_gen: {where}: {cp:#x} outside the BMP")
            keys[kc][col] = cp
        caps[kc] = bool(eval_value(args[5], where))

    dead2, dead3 = [], []
    for args in find_calls(src, "XDEAD"):
        vals = [eval_value(a, where) for a in args]
        if len(vals) == 3:
            dead2.append(vals)
        elif len(vals) == 4:
            dead3.append(vals)
        else:
            raise SystemExit(f"keyboard_layout_gen: {where}: XDEAD takes three or four")

    used = 3 * len(dead2) + 4 * len(dead3) + 2
    if used > DEADKEY_CACHE_SIZE:
        raise SystemExit(f"keyboard_layout_gen: {where}: dead keys need {used} cache "
                         f"bytes, keyboard.c has {DEADKEY_CACHE_SIZE}")

    return {"name": name, "desc": desc, "keys": keys, "caps": caps,
            "dead2": dead2, "dead3": dead3}


def parse_manifest(manifest):
    """The def/keyboard.def include list, which is the layout order."""
    src = strip_comments(Path(manifest).read_text(encoding="utf-8"))
    base = Path(manifest).parent
    names = re.findall(r'#\s*include\s+"core/def/(keyboard_[A-Za-z0-9_]+\.def)"', src)
    if not names:
        raise SystemExit("keyboard_layout_gen: no layouts included by keyboard.def")
    return [parse_layout(base / n) for n in names]


def pack_string(s, size):
    b = s.encode("ascii").ljust(size, b"\0")
    return [b[i] | (b[i + 1] << 8) for i in range(0, size, 2)]


def build(manifest):
    layouts = parse_manifest(manifest)
    n = len(layouts)

    # An interact menu list holds sixteen options, and the Pocket's
    # keyboard menu is one option per layout. That is the ceiling on
    # this manifest, and it is worth failing here rather than in a JSON
    # file the host silently rejects.
    if n > 16:
        raise SystemExit(f"keyboard_layout_gen: {n} layouts, and the Pocket's "
                         "keyboard menu holds sixteen")

    records = []
    for lay in layouts:
        caps = [0] * CAPS_WORDS
        for kc, on in enumerate(lay["caps"]):
            if on:
                caps[kc >> 4] |= 1 << (kc & 15)
        rec = pack_string(lay["name"], NAME_MAX)
        rec += pack_string(lay["desc"], DESC_MAX)
        rec += caps
        rec += [len(lay["dead2"]), len(lay["dead3"])]
        for row in lay["keys"]:
            rec += row
        for e in lay["dead2"]:
            rec += e
        for e in lay["dead3"]:
            rec += e
        records.append(rec)

    at = 2 + n
    offsets = []
    for rec in records:
        offsets.append(at)
        at += len(rec)

    words = [MAGIC, n] + offsets
    for rec in records:
        words += rec
    return words, layouts


HEADER = """/* Generated by src/core/gen/keyboard_layout_gen.py — do not edit.
 *
 * The keyboard layouts as a flat array of 16-bit words, laid out for a
 * reader that can only fetch one word at a time:
 *
 *   [0] magic, [1] layout count
 *   [2 ..]     one record offset per layout, in words
 *
 * and per record, in order: the name and the description as bytes, the
 * caps lock bitmap, the two dead key counts, the code points four to a
 * keycode, then the dead key tables.
 *
 * src/core/hid/layout.c takes the record offsets from the header and the
 * rest from the fixed shape above, so a machine reading this out of a
 * file does not need this comment.
 */
"""


def emit_h(path, words, layouts):
    Path(path).write_text(
        HEADER + f"""
#ifndef _KBDLAY_H_
#define _KBDLAY_H_

#define KBDLAY_MAGIC 0x{MAGIC:04X}u
#define KBDLAY_LAYOUTS {len(layouts)}
#define KBDLAY_WORDS {len(words)}
#define KBDLAY_BYTES {len(words) * 2}

#endif /* _KBDLAY_H_ */
""")


def emit_c(path, words):
    # Placed where the host puts cold tables: flash on a RIA, whose
    # binary is copied to RAM at boot, and nothing anywhere else. The
    # Pocket does not compile this file at all -- its copy lives in the
    # staging store.
    out = [HEADER,
           '\n#include "kbdlay.h"\n#include "core/hid/layout.h"\n'
           '\n#include "host.h"\n\n',
           'static const HOST_IN_FLASH("kbdlay") uint16_t'
           " kbdlay_data[KBDLAY_WORDS] = {\n"]
    for i in range(0, len(words), 12):
        row = ", ".join(f"0x{w:04X}" for w in words[i:i + 12])
        out.append(f"    {row},\n")
    out.append("};\n")
    out.append("""
uint16_t layout_word(uint32_t index)
{
    return index < KBDLAY_WORDS ? kbdlay_data[index] : 0;
}
""")
    Path(path).write_text("".join(out))


def emit_bin(path, words):
    b = bytearray()
    for w in words:
        b += bytes((w & 0xFF, w >> 8))
    Path(path).write_bytes(b)


DEFAULT_LAYOUT = "US"  # keyboard.c falls back to this; the menu must agree


def check_interact(path, layouts):
    """The menu picks a layout by position, so the two must agree."""
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    variables = doc["interact"]["variables"]
    want = [(i + 1, lay["name"]) for i, lay in enumerate(layouts)]
    found = [v for v in variables if v["name"] == "Keyboard"]
    if len(found) != 1:
        raise SystemExit("keyboard_layout_gen: interact.json has no 'Keyboard'")
    options = found[0]["options"]
    got = [(int(o["value"], 0), o["name"]) for o in options]
    if got != want:
        raise SystemExit(f"keyboard_layout_gen: interact.json 'Keyboard' options "
                         f"{got} do not match def/keyboard.def {want}")

    # defaultval indexes the options array — it is not one of their
    # values. Analogue's page does not say so and its own sample is the
    # only evidence; a Pocket booted with the value there came up on the
    # layout one past the intended one. What the default has to be is
    # whatever keyboard.c falls back to when nothing has been chosen.
    default = found[0]["defaultval"]
    if isinstance(default, str):
        raise SystemExit("keyboard_layout_gen: interact.json 'Keyboard' defaultval "
                         f"is the string {default!r}; it is an index")
    if not 0 <= default < len(options):
        raise SystemExit(f"keyboard_layout_gen: interact.json 'Keyboard' defaultval "
                         f"{default} is not an option index")
    if options[default]["name"] != DEFAULT_LAYOUT:
        raise SystemExit(f"keyboard_layout_gen: interact.json 'Keyboard' defaults to "
                         f"{options[default]['name']}, and keyboard.c to "
                         f"{DEFAULT_LAYOUT}")


def check_data(path, words):
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    slots = doc["data"]["data_slots"]
    found = [s for s in slots if s.get("filename") == "keyboard.bin"]
    if len(found) != 1:
        raise SystemExit("keyboard_layout_gen: data.json has no keyboard.bin slot")
    size = int(found[0]["size_exact"], 0)
    if size != len(words) * 2:
        raise SystemExit(f"keyboard_layout_gen: data.json says {size} bytes, "
                         f"the layouts are {len(words) * 2}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--emit-bin")
    ap.add_argument("--emit-c")
    ap.add_argument("--emit-h")
    ap.add_argument("--check-interact")
    ap.add_argument("--check-data")
    a = ap.parse_args()

    words, layouts = build(a.manifest)
    if a.emit_h:
        emit_h(a.emit_h, words, layouts)
    if a.emit_c:
        emit_c(a.emit_c, words)
    if a.emit_bin:
        emit_bin(a.emit_bin, words)
    if a.check_interact:
        check_interact(a.check_interact, layouts)
    if a.check_data:
        check_data(a.check_data, words)
    print(f"kbdlay {len(words)} words, {len(words) * 2} bytes "
          f"({len(layouts)} layouts: "
          f"{' '.join(lay['name'] for lay in layouts)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
