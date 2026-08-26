#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Enough 65C02 to write the programs the generators are for.
#
# The instruction set is not typed out here. It is derived from
# vendor/chips/codegen/w65c02_gen.py, the same table src/core/gen/w65c02_rom_gen.py
# reads to build the RTL's decode ROM and the same one the C emulation is
# generated from. Three implementations of this processor already agree about
# which opcode is which because they read one table; an assembler that retyped
# forty of them would be the first that could disagree.
#
# Deriving it also settles an argument the old file made. It carried seventeen
# instructions and said zero page was left out because it "would save bytes
# nobody is counting and cost a second spelling of every instruction" — but the
# second spelling is what costs nothing once the table is doing the spelling.
# A program at $0300 talking to $FFxx still has no reason to use zero page.
# It is here because excluding it would now be the work.
#
# Names are mnemonic_mode: lda_imm, lda_abs, lda_abx, sta_abx, cmp_imm. The
# nine relative branches and the implied instructions have only one mode each,
# so they are spelled bne and clc rather than bne_rel and clc_imp.
#
# tests/bench/tb_asm.h is a much smaller 65C02 in C++, for benches that build a
# program from a C++ value at run time. It is not this file in another
# language and does not track it; what the two share is checked by
# tests/cpu/ria/test_asm.cpp.

import argparse
import importlib.util
import os
import sys
from contextlib import contextmanager

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CODEGEN = os.path.join(ROOT, "vendor", "chips", "codegen")

# Where a program is loaded and started.
ORG = 0x0300

# The RIA register window, as the 6502 sees it.
RIA_READY = 0xFFE0
RIA_TX = 0xFFE1
RIA_RX = 0xFFE2
RW0_DATA = 0xFFE4
RW0_ADDR = 0xFFE6
XSTACK = 0xFFEC
API_ERRNO = 0xFFED
API_OP = 0xFFEF
API_CALL = 0xFFF1
API_A = 0xFFF4
API_X = 0xFFF6

# The API ops a generated program reaches for. These are the machine's
# numbers, not any one program's, which is why they are not in the
# generator that happened to need them first.
OP_XREG = 0x01
OP_OPEN = 0x14
OP_CLOSE = 0x15
OP_READ_XSTACK = 0x16
OP_WRITE_XSTACK = 0x18
OP_LSEEK = 0x1A
OP_SYNCFS = 0x1E
OP_CHDIR = 0x29
OP_CHDRIVE = 0x2A
OP_GETCWD = 0x2B
OP_GMTIME = 0x3A
OP_LOCALTIME = 0x3B
OP_TIME_GET = 0x3F

O_RDONLY = 0x01
O_WRONLY = 0x02
O_RDWR = 0x03
O_CREAT = 0x10
O_TRUNC = 0x20
O_APPEND = 0x40
O_EXCL = 0x80

# cc65's spelling, not POSIX's.
SEEK_CUR, SEEK_END, SEEK_SET = 0, 1, 2


class AsmError(Exception):
    """A program that cannot be assembled."""


def _dasm():
    """The vendored decode table, loaded by path so its name cannot
    collide — the same way w65c02_rom_gen.py reaches it."""
    path = os.path.join(CODEGEN, "w65c02_gen.py")
    if not os.path.exists(path):
        raise AsmError(
            "vendor/chips is empty; the instruction set is read from it.\n"
            "  git submodule update --init vendor/chips")
    sys.path.insert(0, CODEGEN)  # for the generator's own imports
    spec = importlib.util.spec_from_file_location("w65c02_gen_vendored", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod.DASM


# How many operand bytes a mode takes, and whether the operand is a
# displacement. The modes left out are the ones nothing writes: NON and the
# NOP fillers, AB3, and ZPR — the BBR/BBS pair takes two operands, one of
# them relative, and no program here wants it.
_WIDTH = {
    "IMP": 0, "ACC": 0,
    "IMM": 1, "ZP": 1, "ZPX": 1, "ZPY": 1, "IZX": 1, "IZY": 1, "IZP": 1,
    "REL": 1,
    "ABS": 2, "ABX": 2, "ABY": 2, "IND": 2, "IAX": 2,
}

# The vendor disambiguates by mnemonic where the 65C02 disambiguates by mode,
# so the accumulator forms and the two indirect jumps get their real spelling
# back. Nothing else is renamed.
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
    """name -> (opcode, operand bytes, relative)."""
    out = {}
    for opcode, (mnemonic, mode) in enumerate(_dasm()):
        if mode not in _WIDTH or mnemonic.startswith("NOP") and mnemonic != "NOP":
            continue
        if mnemonic == "NOP" and mode != "IMP":
            continue  # the filler NOPs are not instructions anyone writes
        name = _name(mnemonic, mode)
        if name not in out:
            out[name] = (opcode, _WIDTH[mode], mode == "REL")
    return out


OPCODES = _table()

# The branches, which is what the branch() block accepts.
BRANCHES = {n: op for n, (op, _, rel) in OPCODES.items() if rel}


class Asm:
    """A 65C02 program under construction, assembled at ORG.

    An address argument may be a name instead of a number. The name is
    resolved when the program is read out, so a jump forward reads exactly
    like a jump back and neither counts bytes.
    """

    def __init__(self, org=ORG):
        self.org = org
        self._b = bytearray()
        self._sym = {}
        self._fix = []  # (offset, name, relative)
        self._n = 0

    # --- symbols ---

    def symbol(self, name):
        """Name the address the next instruction assembles at."""
        if name in self._sym:
            raise AsmError(f"symbol already defined: {name} "
                           f"at ${self._sym[name]:04X}")
        self._sym[name] = self.org + len(self._b)
        return name

    def local(self, hint="L"):
        """A name nothing else will produce, for a routine emitted more
        than once."""
        self._n += 1
        return f"{hint}.{self._n}"

    def data(self, by):
        """Literal bytes: a table the program indexes, or a string after the
        code. Not an instruction, which is why it is not an instruction."""
        self._b += bytes(by)

    def here(self):
        """The address the next instruction assembles at.

        Prefer symbol(). This is for the two places that want the number
        itself — a jump to self, and a length."""
        return self.org + len(self._b)

    # --- the encoding ---

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
        """A conditional branch over the block that follows. The
        displacement is whatever the block turned out to be, so nothing
        counts bytes and nothing can be left open.

        Only the real branches are accepted. This used to take an opcode,
        and one generator passed BVC for an unconditional jump on the
        reasoning that V was clear — V being whatever the last BIT $FFE0 in
        putc left there, so a keypress mid-run turned every later pass into
        a failure. The 65C02 has bra."""
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
        """The program, with every reference resolved."""
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

    # --- the machine ---

    def store(self, a, v):
        self.lda_imm(v)
        self.sta_abs(a)

    def inc16(self, lo, hi):
        """++[hi:lo], carrying without disturbing A."""
        self.inc_abs(lo)
        with self.branch("bne"):
            self.inc_abs(hi)

    def push(self, v):
        """The xstack grows down, so the last byte pushed is the first
        the API pops: a word goes high byte first, a string backwards."""
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
        """latin-1, which is to say the byte you wrote. The machine reads
        a filename in its code page, so str.encode()'s UTF-8 default
        would push two bytes for anything over 0x7F and hand the API a
        name it never spelled."""
        if isinstance(s, str):
            s = s.encode("latin-1")
        for c in reversed(s + b"\0"):
            self.push(c)

    def call(self, op):
        """An API call: the op, then the trampoline the 6502 spins in."""
        self.store(API_OP, op)
        self.jsr_abs(API_CALL)

    def call_a(self, op, a):
        """With the attribute id, which every op that takes one puts in A."""
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
        """One XRAM byte through RW0, which is the port a device snoops."""
        self.store(RW0_ADDR, addr & 0xFF)
        self.store(RW0_ADDR + 1, addr >> 8)
        self.store(RW0_DATA, val)

    def putc_a(self):
        """Print the accumulator, waiting for the TX ready bit. Inline,
        for a program with nothing else to print."""
        wait = self.local("putc_a")
        self.pha()
        self.symbol(wait)
        self.bit_abs(RIA_READY)
        self.bpl(wait)
        self.pla()
        self.sta_abs(RIA_TX)

    # --- the subroutine library ---

    def use(self, *subs):
        """Assemble each routine once and name it after itself. Order is
        what you ask for; a routine that calls another does not care
        whether it came first, because a name is resolved at the end."""
        for sub in subs:
            name = sub.__name__
            if name in self._sym:
                continue
            self.symbol(name)
            sub(self)

    def say(self, s):
        """A string through putc, a byte at a time."""
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
    """A to the console, waiting out the TX-ready bit."""
    p.pha()
    p.symbol("putc.wait")
    p.bit_abs(RIA_READY)
    p.bpl("putc.wait")
    p.pla()
    p.sta_abs(RIA_TX)
    p.rts()


def putnib(p):
    """The low nibble of A as one hex digit. A above $0F is the caller's
    problem; puthex masks before it gets here."""
    p.cmp_imm(0x0A)
    with p.branch("bcs"):
        p.clc()
        p.adc_imm(ord("0"))
        p.jmp_abs("putc")
    p.clc()
    p.adc_imm(ord("A") - 10)
    p.jmp_abs("putc")


def puthex(p):
    """A as two hex digits."""
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
    """A high, X low, through one scratch byte the caller names. A factory
    because a routine in a library cannot know where the caller's scratch
    is, and python is a better place to say it than a macro would be."""
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
        # A handful of opcodes with known encodings, so a vendor bump that
        # renames or reshuffles the table is caught here rather than in a
        # ROM that still assembles and no longer runs.
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
