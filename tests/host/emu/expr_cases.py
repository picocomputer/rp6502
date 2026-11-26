#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""This script writes the cases for test_expr.c, one `<want> <expr>` line per
case. Each `<want>` is the answer Python gives when every whole number must
fit in 64 bits. Floats are not allowed, so a float literal, a / or a
negative power is an error.

address(), define(), sizeof() and countof() take text, such as a path with
/ and ., not an expression, so Python cannot read them. Before Python reads
a case, each of them becomes a name with the value that test_expr.c gives
it."""

import argparse
import ast
import io
import operator
import random
import re
import tokenize
import types
import warnings

# test_expr.c has the same names.
NAMES = {
    "a": 7,
    "b": -3,
    "p.q": 0x1234,
    "INT64_MAX": 2**63 - 1,
    "INT64_MIN": -2**63,
    "address(a)": 7,
    "define(b)": -3,
    "sizeof(t)": 12,
    "countof(t)": 6,
    "sizeof(src/m.c/t)": 12,
    "countof(src/m.c/t)": 6,
    "define(src/m.c/p.q)": 0x1234,
}

# The lookups that test_expr.c refuses, as for a #define's address.
REFUSED = {"address(b)", "define(a)", "sizeof(b)", "countof(a)"}

# A lookup, with spaces allowed before the ( and around the text.
FORM = re.compile(r"(?<![\w.$])(address|define|sizeof|countof)[ \t]*\(([^)]*)\)")

EDGES = [
    "INT64_MIN // -1", "INT64_MIN % -1", "-INT64_MIN", "INT64_MIN * -1",
    "INT64_MAX + 1", "INT64_MIN - 1", "9223372036854775807",
    "9223372036854775808", "-1 << 63", "1 << 63", "0 << -1", "0 << 1000",
    "1 << 64", "-1 >> 100", "5 >> 64", "1 >> -1", "010", "00", "0_0",
    "0_1", "1 // 0 + nope", "nope + 1 // 0", "-2 ** 2", "2 ** -1",
    "0 ** -1", "(-8) ** -1", "1 ** -1", "2 ** 3 ** 2", "0 ** 0",
    "(-2) ** 63", "2 ** 62", "2 ** 63", "(-2) ** 64", "(-1) ** INT64_MAX",
    "2 ** INT64_MAX", "INT64_MIN ** 1", "1 < 2 < 3", "1 < 3 < 2",
    "1 < 0 < 1 // 0", "0 and 1 // 0", "1 or nope", "nope or 1", "not 0",
    "not 5", "2 or 3 and 0", "0 or 0", "0x_FF", "$FF_FF", "$_FF", "0b_101",
    "0o17", "0B1", "0X1f", "0x1e3", "$1E3", "1_000", "1__000", "1_", "0x",
    "$", "5 % -3", "-5 // 3", "-7 // 2", "-7 % 2", "p.q + 1", "a + b * a",
    "6 / 2", "1 / 0", "nope / 2", "1 // 0 + 1 / 2", "1 or 1 / 2",
    "1.5 * 2", "2 * 1.5", "1 / 2 + 1.5", "1.5 + 1 / 2", ".5e1", "1.e5",
    "1.", ".5", "2.5e-3", "1E3", "1e+", "010.5", "01e1", "0_9.5", "1e309",
    "1 and.5", "0 or.5", "not.5", "1e3 / (1", "(1", "1)", "()", "", "1 1",
    "a +", "not", "- not 1", "1 < not 1", "f(1)", "1 if 1 else 2", "1 = 1",
    "\"a\"", "1j", "sizeof(t) + 1", "countof(src/m.c/t) * 2",
    "sizeof( src/m.c/t ) - countof (t)", "address(a) // 2", "define(src/m.c/p.q)",
    "define(a)", "1 or define(a)", "sizeof(nope)", "sizeof(src/x.c/t)",
    "sizeof()", "sizeof( )", "sizeof(t", "(sizeof(t)", "sizeof(t))",
    "sizeof((t)", "sizeof(t)(1)", "sizeof", "sizeof + 1", "p.sizeof(t)",
    "sizeof.x(t)", "notsizeof(t)", "not sizeof(t)", "f(src/m.c/t)",
    "sizeof(src/m.c/t)/2", "2 ** countof(t)", "-sizeof(b)", "count(t)",
    "sizeof(t)and 1", "sizeof(t)or 0", "sizeof(t)1", "sizeof(t)sizeof(t)",
    "$sizeof(t)", "1sizeof(t)", "sizeof(t)if 1 else 2",
]

UNARY = {ast.USub: operator.neg, ast.UAdd: operator.pos,
         ast.Invert: operator.invert}
BINARY = {ast.Add: operator.add, ast.Sub: operator.sub,
          ast.Mult: operator.mul, ast.Div: operator.truediv,
          ast.FloorDiv: operator.floordiv, ast.Mod: operator.mod,
          ast.Pow: operator.pow, ast.LShift: operator.lshift,
          ast.RShift: operator.rshift, ast.BitAnd: operator.and_,
          ast.BitOr: operator.or_, ast.BitXor: operator.xor}
COMPARE = {ast.Lt: operator.lt, ast.LtE: operator.le, ast.Gt: operator.gt,
           ast.GtE: operator.ge, ast.Eq: operator.eq, ast.NotEq: operator.ne}

ERRORS = {NameError: "unknown", ZeroDivisionError: "zero",
          OverflowError: "overflow", ValueError: "shift"}


class Big(Exception):
    """A whole number that does not fit in 64 bits."""


class Power(Exception):
    """A negative power, which gives a float in Python."""


class Refused(Exception):
    """A lookup that test_expr.c refuses."""


def forms(text):
    """The text with each lookup as a name (_f<n>), and a dict of those names
    to the lookups. The parentheses keep the name one atom, apart from what
    is next to it. A lookup without text stays, as a call that is bad."""
    found = {}

    def name(m):
        inside = m.group(2).strip(" \t")
        if not inside:
            return m.group(0)
        found[f"_f{len(found)}"] = f"{m.group(1)}({inside})"
        return f"(_f{len(found) - 1})"

    return FORM.sub(name, text), found


def dotted(n):
    if isinstance(n, ast.Name):
        return n.id
    if isinstance(n, ast.Attribute) and dotted(n.value):
        return dotted(n.value) + "." + n.attr
    return None


def supported(n):
    if isinstance(n, ast.Constant):
        return type(n.value) in (int, float)
    if isinstance(n, (ast.Name, ast.Attribute)):
        return dotted(n) is not None
    if isinstance(n, ast.UnaryOp):
        return supported(n.operand)
    if isinstance(n, ast.BinOp):
        return type(n.op) in BINARY and supported(n.left) and supported(n.right)
    if isinstance(n, ast.BoolOp):
        return all(supported(v) for v in n.values)
    if isinstance(n, ast.Compare):
        return (all(type(op) in COMPARE for op in n.ops)
                and all(supported(v) for v in [n.left] + n.comparators))
    return False


def fit(v):
    if not -2**63 <= v < 2**63:
        raise Big
    return v


def walk(n, lookups):
    """Works n out in Python's order, stopping at the first error."""
    if isinstance(n, ast.Constant):
        return fit(n.value)
    if isinstance(n, (ast.Name, ast.Attribute)):
        name = lookups.get(dotted(n), dotted(n))
        if name in REFUSED:
            raise Refused
        if name not in NAMES:
            raise NameError(name)
        return NAMES[name]
    if isinstance(n, ast.UnaryOp):
        v = walk(n.operand, lookups)
        if isinstance(n.op, ast.Not):
            return int(not v)
        return fit(UNARY[type(n.op)](v))
    if isinstance(n, ast.BinOp):
        a, b = walk(n.left, lookups), walk(n.right, lookups)
        op = type(n.op)
        # Python would build a huge number before the check.
        if op is ast.Pow and b >= 64 and abs(a) >= 2:
            raise Big
        if op is ast.LShift and b >= 64 and a:
            raise Big
        if op is ast.Pow and b < 0 and a:
            raise Power
        return fit(BINARY[op](a, b))
    if isinstance(n, ast.BoolOp):
        v = walk(n.values[0], lookups)
        for x in n.values[1:]:
            if bool(v) == isinstance(n.op, ast.Or):
                break
            v = walk(x, lookups)
        return v
    a = walk(n.left, lookups)
    for op, x in zip(n.ops, n.comparators):
        b = walk(x, lookups)
        if not COMPARE[type(op)](a, b):
            return 0
        a = b
    return 1


def show(v):
    if type(v) is float:
        return "float"
    return f"int:{int(v)}"


def refused(text):
    """divide or float for the first / or float literal, else None. These are
    errors before anything is worked out."""
    lines = io.StringIO(text.replace("$", "0x")).readline
    for tok in tokenize.generate_tokens(lines):
        if tok.type == tokenize.OP and tok.string == "/":
            return "divide"
        number = tok.string.lower()
        if (tok.type == tokenize.NUMBER and number[:2] not in ("0x", "0o", "0b")
                and ("." in number or "e" in number)):
            return "float"
    return None


def answer(text):
    """The want text, and whether eval() should give it too."""
    text, lookups = forms(text)
    try:
        tree = ast.parse(text.replace("$", "0x"), mode="eval")
    except SyntaxError:
        return "bad", True
    if not supported(tree.body):
        return "bad", False
    why = refused(text)
    if why:
        return why, False
    try:
        return show(walk(tree.body, lookups)), True
    except Big:
        return "overflow", False
    except Power:
        return "float", False
    except Refused:
        return "refused", False
    except tuple(ERRORS) as e:
        return ERRORS[type(e)], True


def python(text):
    text, lookups = forms(text)
    names = {k: v for k, v in NAMES.items() if "." not in k}
    names["p"] = types.SimpleNamespace(q=NAMES["p.q"])
    names.update({k: NAMES[v] for k, v in lookups.items() if v in NAMES})
    try:
        return show(eval(text.replace("$", "0x"), {"__builtins__": {}}, names))
    except SyntaxError:
        return "bad"
    except tuple(ERRORS) as e:
        return ERRORS[type(e)]


def underscores(r, digits):
    if len(digits) < 2 or r.random() < 0.7:
        return digits
    cut = sorted(r.sample(range(1, len(digits)), r.randint(1, len(digits) - 1)))
    return "_".join(digits[i:j] for i, j in zip([0] + cut, cut + [None]))


WHOLES = [0, 1, 2, 3, 5, 7, 10, 31, 32, 53, 62, 63, 64, 65, 100, 255, 256,
          2**31, 2**32, 2**53, 2**53 + 1, 2**62, 2**63 - 1, 2**63, 2**64 - 1,
          2**64, 10**20]


def whole(r):
    n = r.choice(WHOLES)
    if r.random() < 0.4:
        n = r.randrange(2**r.randint(1, 64))
    form = r.randrange(5)
    if form == 0:
        text = underscores(r, str(n))
        return "0" + text if r.random() < 0.05 else text
    prefix, digits = [("0x", f"{n:x}"), ("$", f"{n:X}"), ("0b", f"{n:b}"),
                      ("0o", f"{n:o}")][form - 1]
    if prefix != "$" and r.random() < 0.2:
        prefix = prefix.upper()
    lead = "_" if r.random() < 0.1 else ""
    return prefix + lead + underscores(r, digits)


def digits(r, least, most):
    return underscores(r, "".join(r.choice("0123456789")
                                  for _ in range(r.randint(least, most))))


def float_text(r):
    form = r.randrange(4)
    if form == 0:
        return repr(r.choice([r.random(), r.uniform(0, 1e6),
                              10 ** r.uniform(-330, 308)]))
    if form == 1:
        return r.choice(["1.", ".5", "1e3", "1E+3", "1.e-3", "2.5e-3",
                         "010.5", "01e1", "00.0", "1e309", "5e-324",
                         "1e-400", "0.1", "1.5", "2.5", "0.5", "3.0"])
    lead, tail = digits(r, 0, 20), digits(r, 0, 20)
    text = lead + "." + tail if lead or tail else "1."
    if form == 3:
        text += r.choice("eE") + r.choice(["", "+", "-"]) + digits(r, 1, 3)
    return text


def atom(r):
    k = r.random()
    if k < 0.7:
        return whole(r)
    if k < 0.73:
        return float_text(r)
    if k < 0.78:
        return r.choice(sorted(NAMES.keys() - {"a", "b", "p.q", "INT64_MAX", "INT64_MIN"})
                        + sorted(REFUSED) + ["sizeof(nope)", "countof (t)"])
    return r.choice(["a", "b", "p.q", "INT64_MAX", "INT64_MIN", "nope"])


# / is rare, because any / makes the whole case an error.
BINOPS = ["or", "and", "<", "<=", ">", ">=", "==", "!=", "|", "^", "&",
          "<<", ">>", "+", "-", "*", "//", "%", "**"]


def expr(r, depth):
    k = r.random()
    if depth == 0 or k < 0.25:
        return atom(r)
    if k < 0.4:
        return r.choice(["-", "+", "~", "not "]) + expr(r, depth - 1)
    if k < 0.5:
        return "(" + expr(r, depth - 1) + ")"
    op = "/" if r.random() < 0.01 else r.choice(BINOPS)
    space = " " if op in ("or", "and") or r.random() < 0.7 else ""
    return expr(r, depth - 1) + space + op + space + expr(r, depth - 1)


def case(text):
    want, check = answer(text)
    if check:
        got = python(text)
        assert got == want, f"{text}: walk() gives {want}, eval() gives {got}"
    return f"{want} {text}\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--emit", required=True)
    ap.add_argument("--seed", type=int, default=6502)
    a = ap.parse_args()
    # Python 3.12 warns about ~ on True or False, which some cases do.
    warnings.simplefilter("ignore", DeprecationWarning)
    lines = [case(text) for text in EDGES]
    seen = set(EDGES)
    r = random.Random(a.seed)
    while len(lines) < len(EDGES) + 20000:
        text = expr(r, r.randint(1, 4))
        if text not in seen:
            seen.add(text)
            lines.append(case(text))
    with open(a.emit, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(lines)
    print(f"expr_cases.txt {len(lines)} cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
