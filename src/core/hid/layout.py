#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The keyboard layouts in core/def for core/hid/layout.c.

import json
import re
from pathlib import Path

import build

MANIFEST = "src/core/def/keyboard.def"

# These are the buffer sizes a caller declares, NUL included.
NAME_MAX = 8
DESC_MAX = 32

# This is the size of the buffer in which core/hid/keymap.c caches the
# dead key tables as OEM characters. keymap.c does not check that they
# fit, so an overflow is refused here when the layout is built.
DEADKEY_CACHE_SIZE = 512

ESCAPES = {"n": 0x0A, "r": 0x0D, "t": 0x09, "b": 0x08, "f": 0x0C,
           "v": 0x0B, "a": 0x07, "0": 0x00, "\\": 0x5C, "'": 0x27,
           '"': 0x22, "?": 0x3F}


def strip_comments(src):
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
        raise SystemExit(f"layout.py: {where}: empty character")
    if body[0] != "\\":
        if len(body) != 1:
            raise SystemExit(f"layout.py: {where}: multi-char {lit}")
        return ord(body)
    esc = body[1]
    if esc == "x":
        return int(body[2:], 16)
    if esc in ESCAPES and len(body) == 2:
        return ESCAPES[esc]
    if esc.isdigit():
        return int(body[1:], 8)
    raise SystemExit(f"layout.py: {where}: bad escape {lit}")


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
        raise SystemExit(f"layout.py: {where}: cannot read {arg}")


def eval_string(arg, where):
    if len(arg) < 2 or arg[0] != '"' or arg[-1] != '"':
        raise SystemExit(f"layout.py: {where}: expected a string, got {arg}")
    return arg[1:-1].encode("ascii").decode("unicode_escape")


def parse_layout(path):
    src = strip_comments(Path(path).read_text(encoding="utf-8"))
    where = Path(path).name

    begin = find_calls(src, "XBEGIN")
    if len(begin) != 1:
        raise SystemExit(f"layout.py: {where}: expected one XBEGIN")
    name = eval_string(begin[0][0], where)
    desc = eval_string(begin[0][1], where)
    if len(name.encode()) >= NAME_MAX:
        raise SystemExit(f"layout.py: {where}: name {name!r} over {NAME_MAX - 1}")
    if len(desc.encode()) >= DESC_MAX:
        raise SystemExit(f"layout.py: {where}: description over {DESC_MAX - 1}")

    keys = [[0] * 4 for _ in range(128)]
    caps = [False] * 128
    for args in find_calls(src, "XKEY"):
        if len(args) != 6:
            raise SystemExit(f"layout.py: {where}: XKEY takes six fields")
        kc = eval_value(args[0], where)
        if not 0 <= kc < 128:
            raise SystemExit(f"layout.py: {where}: keycode {kc:#x} over 0x7F")
        for col in range(4):
            cp = eval_value(args[1 + col], where)
            if cp > 0xFFFF:
                raise SystemExit(f"layout.py: {where}: {cp:#x} outside the BMP")
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
            raise SystemExit(f"layout.py: {where}: XDEAD takes three or four")

    used = 3 * len(dead2) + 4 * len(dead3) + 2
    if used > DEADKEY_CACHE_SIZE:
        raise SystemExit(f"layout.py: {where}: dead keys need {used} cache "
                         f"bytes, keymap.c has {DEADKEY_CACHE_SIZE}")

    return {"name": name, "desc": desc, "keys": keys, "caps": caps,
            "dead2": dead2, "dead3": dead3}


def layout_paths():
    src = strip_comments((build.ROOT / MANIFEST).read_text(encoding="utf-8"))
    names = re.findall(r'#\s*include\s+"core/def/(keyboard_[A-Za-z0-9_]+\.def)"', src)
    if not names:
        raise SystemExit("layout.py: no layouts included by keyboard.def")
    return [(Path(MANIFEST).parent / n).as_posix() for n in names]


def load_layouts(paths):
    layouts = [parse_layout(build.ROOT / p) for p in paths]
    if len(layouts) > 16:
        raise SystemExit(f"layout.py: {len(layouts)} layouts, and the "
                         "Pocket's keyboard menu holds sixteen")
    if DEFAULT_LAYOUT not in [lay["name"] for lay in layouts]:
        raise SystemExit(f"layout.py: no {DEFAULT_LAYOUT} layout, which "
                         "layout.c adopts on a machine with no list")
    return layouts


HEADER = """/* Generated by src/core/hid/layout.py -- do not edit.
 *
 * The keyboard layouts of src/core/hid/layout.c, in the order
 * src/core/def/keyboard.def includes them.
 */
"""


def emit_h(path, layouts):
    build.write_text(path,
        HEADER + f"""
#ifndef _LAYOUT_DB_H_
#define _LAYOUT_DB_H_

#include <stdint.h>

#define LAYOUT_DB_LAYOUTS {len(layouts)}

/* caps has a bit for each keycode that caps lock shifts. keys holds the
 * code points, four to a keycode. dead holds the two-key dead entries,
 * three code points each, then the three-key entries, four each. */
typedef struct
{{
    char name[{NAME_MAX}];
    char description[{DESC_MAX}];
    uint16_t caps[8];
    uint16_t dead2_count;
    uint16_t dead3_count;
    const uint16_t *dead;
    uint16_t keys[128][4];
}} layout_db_t;

extern const layout_db_t layout_db[LAYOUT_DB_LAYOUTS];

#endif /* _LAYOUT_DB_H_ */
""")


def c_string(s):
    return '"' + "".join(chr(b) if chr(b).isalnum() or chr(b) in " -_.,()/+"
                         else f"\\{b:03o}" for b in s.encode("ascii")) + '"'


def emit_c(path, layouts):
    # On a RIA the tables stay in flash.
    out = [HEADER, '\n#include "layout_db.h"\n\n#include "machine.h"\n']
    for n, lay in enumerate(layouts):
        dead = [w for e in lay["dead2"] + lay["dead3"] for w in e]
        if not dead:
            continue
        out.append(f"\n/* {lay['name']} */\nstatic const HOST_IN_FLASH "
                   f"uint16_t layout_db_dead_{n}[] = {{\n")
        for i in range(0, len(dead), 12):
            row = ", ".join(f"0x{w:04X}" for w in dead[i:i + 12])
            out.append(f"    {row},\n")
        out.append("};\n")
    out.append("\nconst HOST_IN_FLASH layout_db_t layout_db[] = {\n")
    for n, lay in enumerate(layouts):
        caps = [0] * 8
        for kc, on in enumerate(lay["caps"]):
            if on:
                caps[kc >> 4] |= 1 << (kc & 15)
        has_dead = lay["dead2"] or lay["dead3"]
        out.append("    {\n")
        out.append(f"        {c_string(lay['name'])},\n")
        out.append(f"        {c_string(lay['desc'])},\n")
        out.append("        {" + ", ".join(f"0x{w:04X}" for w in caps) + "},\n")
        out.append(f"        {len(lay['dead2'])},\n")
        out.append(f"        {len(lay['dead3'])},\n")
        out.append(f"        {f'layout_db_dead_{n}' if has_dead else 0},\n")
        out.append("        {\n")
        for i in range(0, 128, 3):
            row = ", ".join("{" + ", ".join(f"0x{w:04X}" for w in k) + "}"
                            for k in lay["keys"][i:i + 3])
            out.append(f"            {row},\n")
        out.append("        },\n    },\n")
    out.append("};\n")
    build.write_text(path, "".join(out))


DEFAULT_LAYOUT = "US"  # core/hid/layout.c adopts this layout.


def check_interact(path, layouts):
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    variables = doc["interact"]["variables"]
    want = [(i + 1, lay["name"]) for i, lay in enumerate(layouts)]
    found = [v for v in variables if v["name"] == "Keyboard"]
    if len(found) != 1:
        raise SystemExit("layout.py: interact.json has no 'Keyboard'")
    options = found[0]["options"]
    got = [(int(o["value"], 0), o["name"]) for o in options]
    if got != want:
        raise SystemExit(f"layout.py: interact.json 'Keyboard' options "
                         f"{got} do not match def/keyboard.def {want}")

    # The Pocket reads defaultval as an index into the options array, not as
    # one of the option values.
    default = found[0]["defaultval"]
    if isinstance(default, str):
        raise SystemExit("layout.py: interact.json 'Keyboard' defaultval "
                         f"is the string {default!r}; it is an index")
    if not 0 <= default < len(options):
        raise SystemExit(f"layout.py: interact.json 'Keyboard' defaultval "
                         f"{default} is not an option index")
    if options[default]["name"] != DEFAULT_LAYOUT:
        raise SystemExit(f"layout.py: interact.json 'Keyboard' defaults to "
                         f"{options[default]['name']}, and layout.c to "
                         f"{DEFAULT_LAYOUT}")


def main():
    paths = layout_paths()
    layouts = load_layouts(paths)

    a = build.generate(inputs=[MANIFEST] + paths,
                 c={"layout_db.h": lambda p: emit_h(p, layouts),
                    "layout_db.c": lambda p: emit_c(p, layouts)},
                 extra=lambda ap: ap.add_argument("--check-interact",
                                                  metavar="JSON"))
    if a.check_interact:
        check_interact(a.check_interact, layouts)


if __name__ == "__main__":
    main()
