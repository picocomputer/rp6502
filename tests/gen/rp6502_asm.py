#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import importlib.util
import os
import sys
from contextlib import contextmanager

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CODEGEN = os.path.join(ROOT, "vendor", "chips", "codegen")

ORG = 0x0300

RIA_READY = 0xFFE0
RIA_TX = 0xFFE1
RIA_RX = 0xFFE2
RW0_DATA = 0xFFE4
RW0_STEP = 0xFFE5
RW0_ADDR = 0xFFE6
RW1_DATA = 0xFFE8
RW1_STEP = 0xFFE9
RW1_ADDR = 0xFFEA
XSTACK = 0xFFEC
API_ERRNO = 0xFFED
API_OP = 0xFFEF
API_CALL = 0xFFF1
API_A = 0xFFF4
API_X = 0xFFF6

VIA_T1_LO = 0xFFD4
VIA_T1_HI = 0xFFD5
VIA_T1L_LO = 0xFFD6
VIA_T1L_HI = 0xFFD7
VIA_ACR = 0xFFDB
VIA_IFR = 0xFFDD
VIA_IER = 0xFFDE

OP_DROP_XSTACK = 0x00
OP_XREG = 0x01
OP_ERRNO_OPT = 0x06
OP_ARGV = 0x08
OP_EXEC = 0x09
OP_ATTR_GET = 0x0A
OP_OPEN = 0x14
OP_CLOSE = 0x15
OP_READ_XSTACK = 0x16
OP_READ_XRAM = 0x17
OP_WRITE_XSTACK = 0x18
OP_WRITE_XRAM = 0x19
OP_LSEEK = 0x1A
OP_UNLINK = 0x1B
OP_RENAME = 0x1C
OP_SYNCFS = 0x1E
OP_STAT = 0x1F
OP_OPENDIR = 0x20
OP_READDIR = 0x21
OP_CLOSEDIR = 0x22
OP_MKDIR = 0x28
OP_CHDIR = 0x29
OP_CHDRIVE = 0x2A
OP_GETCWD = 0x2B
OP_GETLABEL = 0x2D
OP_GETFREE = 0x2E
OP_GMTIME = 0x3A
OP_LOCALTIME = 0x3B
OP_STRFTIME = 0x3D
OP_TIME_GET = 0x3F
OP_EXIT = 0xFF

O_RDONLY = 0x01
O_WRONLY = 0x02
O_RDWR = 0x03
O_CREAT = 0x10
O_TRUNC = 0x20
O_APPEND = 0x40
O_EXCL = 0x80

# These are cc65's whence values, which differ from POSIX's.
SEEK_CUR, SEEK_END, SEEK_SET = 0, 1, 2


class AsmError(Exception):
    """AsmError is raised when a program cannot be assembled, and when
    vendor/chips/codegen/w65c02_gen.py, the source of the instruction
    table, is missing."""


def _dasm():
    path = os.path.join(CODEGEN, "w65c02_gen.py")
    if not os.path.exists(path):
        raise AsmError(
            "vendor/chips is empty; the instruction set is read from it.\n"
            "  git submodule update --init vendor/chips")
    sys.path.insert(0, CODEGEN)
    spec = importlib.util.spec_from_file_location("w65c02_gen_vendored", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod.DASM


_WIDTH = {
    "IMP": 0, "ACC": 0,
    "IMM": 1, "ZP": 1, "ZPX": 1, "ZPY": 1, "IZX": 1, "IZY": 1, "IZP": 1,
    "REL": 1,
    "ABS": 2, "ABX": 2, "ABY": 2, "IND": 2, "IAX": 2,
}

_SPELL = {
    "ASLA": "asl_a", "LSRA": "lsr_a", "ROLA": "rol_a", "RORA": "ror_a",
    "INCA": "inc_a", "DECA": "dec_a",
    "JMPI": "jmp_ind", "JMPX": "jmp_iax", "BITIMM": "bit_imm",
}


def _name(mnemonic, mode):
    if mnemonic in _SPELL:
        return _SPELL[mnemonic]
    lower = mnemonic.lower()
    if mode in ("IMP", "REL"):
        return lower
    return f"{lower}_{mode.lower()}"


def _table():
    out = {}
    for opcode, (mnemonic, mode) in enumerate(_dasm()):
        if mode not in _WIDTH or mnemonic.startswith("NOP") and mnemonic != "NOP":
            continue
        if mnemonic == "NOP" and mode != "IMP":
            continue
        name = _name(mnemonic, mode)
        if name not in out:
            out[name] = (opcode, _WIDTH[mode], mode == "REL")
    return out


OPCODES = _table()

BRANCHES = {n: op for n, (op, _, rel) in OPCODES.items() if rel}


class Asm:
    def __init__(self, org=ORG):
        self.org = org
        self._b = bytearray()
        self._sym = {}
        self._fix = []
        self._n = 0

    def symbol(self, name):
        if name in self._sym:
            raise AsmError(f"symbol already defined: {name} "
                           f"at ${self._sym[name]:04X}")
        self._sym[name] = self.org + len(self._b)
        return name

    def local(self, hint="L"):
        self._n += 1
        return f"{hint}.{self._n}"

    def data(self, by):
        self._b += bytes(by)

    def here(self):
        return self.org + len(self._b)

    def _byte(self, name, v):
        if not isinstance(v, int) or not 0 <= v <= 0xFF:
            raise AsmError(f"{name}: {v!r} is not a byte")
        return v

    def _addr(self, name, v):
        if isinstance(v, str):
            self._fix.append((len(self._b), v, False))
            return 0, 0
        if not isinstance(v, int) or not 0 <= v <= 0xFFFF:
            raise AsmError(f"{name}: {v!r} is not an address")
        return v & 0xFF, v >> 8

    def _rel(self, name, v):
        if isinstance(v, str):
            self._fix.append((len(self._b), v, True))
            return 0
        raise AsmError(f"{name}: a branch goes to a symbol, not {v!r}")

    @contextmanager
    def branch(self, mnemonic):
        if mnemonic not in BRANCHES:
            raise AsmError(f"{mnemonic} is not a branch")
        self._b += bytes((BRANCHES[mnemonic], 0))
        at = len(self._b) - 1
        yield
        d = len(self._b) - at - 1
        if d > 127:
            raise AsmError(f"{mnemonic} over {d} bytes, out of reach")
        self._b[at] = d

    def code(self):
        end = self.org + len(self._b)
        if end > 0x10000:
            raise AsmError(f"program runs past $FFFF: "
                           f"{len(self._b)} bytes at ${self.org:04X}")
        missing = sorted({n for _, n, _ in self._fix if n not in self._sym})
        if missing:
            raise AsmError("undefined symbol: " + ", ".join(missing))
        for at, name, rel in self._fix:
            target = self._sym[name]
            if rel:
                d = target - (self.org + at + 1)
                if not -128 <= d <= 127:
                    raise AsmError(
                        f"branch to {name} at ${target:04X} is {d} bytes "
                        f"from ${self.org + at + 1:04X}, out of reach")
                self._b[at] = d & 0xFF
            else:
                self._b[at] = target & 0xFF
                self._b[at + 1] = target >> 8
        return bytes(self._b)

    def __len__(self):
        return len(self._b)

    def __getattr__(self, name):
        raise AsmError(f"no such instruction: {name}")

    def store(self, a, v):
        self.lda_imm(v)
        self.sta_abs(a)

    def inc16(self, lo, hi):
        self.inc_abs(lo)
        with self.branch("bne"):
            self.inc_abs(hi)

    def push(self, v):
        """The xstack grows down, so the last byte pushed is at the lowest
        address, and the API reads a value upward from that address. A word
        is therefore pushed high byte first, and a string is pushed
        backwards."""
        self.store(XSTACK, v)

    def pushw(self, w):
        self.push((w >> 8) & 0xFF)
        self.push(w & 0xFF)

    def pushl(self, v):
        self.push((v >> 24) & 0xFF)
        self.push((v >> 16) & 0xFF)
        self.push((v >> 8) & 0xFF)
        self.push(v & 0xFF)

    def push_str(self, s):
        if isinstance(s, str):
            s = s.encode("latin-1")
        for c in reversed(s + b"\0"):
            self.push(c)

    def call(self, op):
        self.store(API_OP, op)
        self.jsr_abs(API_CALL)

    def call_a(self, op, a):
        self.store(API_A, a)
        self.call(op)

    def xreg(self, dev, ch, addr, *words):
        self.push(dev)
        self.push(ch)
        self.push(addr)
        for w in words:
            self.pushw(w)
        self.call(OP_XREG)

    def poke(self, addr, val):
        self.store(RW0_ADDR, addr & 0xFF)
        self.store(RW0_ADDR + 1, addr >> 8)
        self.store(RW0_DATA, val)

    def putc_a(self):
        wait = self.local("putc_a")
        self.pha()
        self.symbol(wait)
        self.bit_abs(RIA_READY)
        self.bpl(wait)
        self.pla()
        self.sta_abs(RIA_TX)

    def use(self, *subs):
        for sub in subs:
            name = sub.__name__
            if name in self._sym:
                continue
            self.symbol(name)
            sub(self)

    def say(self, s):
        for c in s.encode("latin-1"):
            self.lda_imm(c)
            self.jsr_abs("putc")


def _bind():
    def make(opcode, width, rel):
        if width == 0:
            def op(self):
                self._b += bytes((opcode,))
        elif rel:
            def op(self, target):
                self._b += bytes((opcode,))
                self._b += bytes((self._rel(op.__name__, target),))
        elif width == 1:
            def op(self, v):
                self._b += bytes((opcode, self._byte(op.__name__, v)))
        else:
            def op(self, a):
                self._b += bytes((opcode,))
                self._b += bytes(self._addr(op.__name__, a))
        return op

    for name, (opcode, width, rel) in OPCODES.items():
        fn = make(opcode, width, rel)
        fn.__name__ = name
        fn.__qualname__ = f"Asm.{name}"
        setattr(Asm, name, fn)


_bind()


def putc(p):
    p.pha()
    p.symbol("putc.wait")
    p.bit_abs(RIA_READY)
    p.bpl("putc.wait")
    p.pla()
    p.sta_abs(RIA_TX)
    p.rts()


def putnib(p):
    p.cmp_imm(0x0A)
    with p.branch("bcs"):
        p.clc()
        p.adc_imm(ord("0"))
        p.jmp_abs("putc")
    p.clc()
    p.adc_imm(ord("A") - 10)
    p.jmp_abs("putc")


def puthex(p):
    p.pha()
    p.lsr_a()
    p.lsr_a()
    p.lsr_a()
    p.lsr_a()
    p.jsr_abs("putnib")
    p.pla()
    p.and_imm(0x0F)
    p.jmp_abs("putnib")


def puthex16(tmp):
    def puthex16(p):
        p.stx_abs(tmp)
        p.jsr_abs("puthex")
        p.lda_abs(tmp)
        p.jmp_abs("puthex")
    return puthex16


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--list", action="store_true",
                    help="every instruction this assembler carries")
    ap.add_argument("--check", action="store_true",
                    help="known opcodes, against the vendor table")
    a = ap.parse_args()
    if a.list:
        for name in sorted(OPCODES):
            opcode, width, rel = OPCODES[name]
            print(f"{opcode:02X} {name:<10} {width}{' rel' if rel else ''}")
        print(f"{len(OPCODES)} instructions")
    if a.check:
        known = {
            "lda_imm": 0xA9, "lda_abs": 0xAD, "lda_abx": 0xBD,
            "ldx_imm": 0xA2, "ldy_imm": 0xA0,
            "sta_abs": 0x8D, "sta_abx": 0x9D, "stx_abs": 0x8E,
            "cmp_imm": 0xC9, "cmp_abs": 0xCD, "cpx_imm": 0xE0,
            "cpx_abs": 0xEC, "ora_abs": 0x0D, "and_imm": 0x29,
            "adc_imm": 0x69, "adc_abs": 0x6D, "sbc_imm": 0xE9,
            "inc_abs": 0xEE, "dec_abs": 0xCE, "lsr_a": 0x4A,
            "bit_abs": 0x2C, "jsr_abs": 0x20, "jmp_abs": 0x4C,
            "clc": 0x18, "sec": 0x38, "pha": 0x48, "pla": 0x68,
            "tax": 0xAA, "txa": 0x8A, "inx": 0xE8, "dex": 0xCA,
            "iny": 0xC8, "dey": 0x88, "nop": 0xEA, "rts": 0x60,
            "stp": 0xDB, "bne": 0xD0, "beq": 0xF0, "bcs": 0xB0,
            "bcc": 0x90, "bpl": 0x10, "bra": 0x80,
        }
        bad = 0
        for name, want in sorted(known.items()):
            got = OPCODES.get(name, (None,))[0]
            if got != want:
                got = "missing" if got is None else f"${got:02X}"
                print(f"{name}: {got}, expected ${want:02X}", file=sys.stderr)
                bad += 1
        if bad:
            return 1
        print(f"{len(known)} known opcodes, {len(OPCODES)} instructions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
