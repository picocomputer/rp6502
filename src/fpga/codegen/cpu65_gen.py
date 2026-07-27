#!/usr/bin/env python3
#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Generate cpu65's decode tables from the same source as the C emulation.
#
# vendor/chips_rp6502 carries the corrected W65C02S generator, and the decode
# table, addressing modes and per-opcode cycle sequences are read from it rather
# than retyped. The RTL then cannot disagree with the emulator about which
# opcode takes how many cycles, and per-cycle lockstep is left to catch only
# what the two genuinely implement differently.
#
# --report prints the action vocabulary the microcode has to cover, derived from
# the C the generator emits.

import argparse
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
OVERRIDE = os.path.join(ROOT, 'vendor', 'chips_rp6502')


def load_gen():
    """The corrected generator, loaded by path so its name cannot collide."""
    spec = importlib.util.spec_from_file_location(
        'w65c02_gen_override', os.path.join(OVERRIDE, 'w65c02_gen.py'))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod.up


def macro_args(src, name):
    """Every argument list of macro `name` in src, paren-balanced."""
    out = []
    for m in re.finditer(r'\b%s\(' % re.escape(name), src):
        i = m.end()
        depth = 1
        start = i
        while i < len(src) and depth:
            if src[i] == '(':
                depth += 1
            elif src[i] == ')':
                depth -= 1
            i += 1
        out.append(src[start:i - 1])
    return out


def split_top(args):
    """Split a macro argument list on top-level commas."""
    parts, depth, cur = [], 0, ''
    for ch in args:
        if ch == ',' and depth == 0:
            parts.append(cur)
            cur = ''
            continue
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        cur += ch
    parts.append(cur)
    return parts


class Tick:
    """The observable shape of one cycle, read out of the emitted C."""

    def __init__(self, src):
        self.src = src
        self.addrs = macro_args(src, '_SA')
        self.data_out = macro_args(src, '_SD')
        for a in macro_args(src, '_SAD'):
            addr, data = split_top(a)
            self.addrs.append(addr)
            self.data_out.append(data)
        self.write = '_WR()' in src
        self.fetch = '_FETCH()' in src
        self.skip = 'c->IR++' in src
        self.conditional = 'if(' in src

    @property
    def holds_addr(self):
        """No new address: the bus keeps what the previous cycle drove."""
        return not self.addrs and not self.fetch


def opcodes(gen):
    for op in range(256):
        o = gen.enc_op(op)
        yield op, gen.DASM[op], [Tick(o.src[t]) for t in range(o.i)]


# The address unit, as the whole instruction set actually uses it. Every
# address is hi:lo, where lo is a source plus an offset and hi is either the
# carry-corrected page or a page held from somewhere else.
#
#   lo   which register or bus value feeds the low byte
#   off  what is added to it
#   hi   where the high byte comes from: CARRY propagates from lo, otherwise the
#        page is held (zero page, the stack page, a branch before fixup)
#   post which register the cycle leaves incremented or decremented
#
# Spelling each of the emitted expressions out by hand keeps this honest: an
# expression the map does not know is a hard error, so an upstream change to the
# addressing cannot slip into the RTL unnoticed.
LO = ('PC', 'AD', 'S', 'GD', 'ZERO', 'HOLD')
OFF = ('0', '1', 'X', 'Y', 'M1', 'M2')
HI = ('CARRY', 'ZP', 'STACK', 'PC', 'AD', 'GD', 'ZERO', 'HOLD')
POST = ('NONE', 'PC_INC', 'AD_INC', 'S_INC', 'S_DEC')

ADDR_MAP = {
    # program counter
    'c->PC': ('PC', '0', 'CARRY', 'NONE'),
    'c->PC++': ('PC', '0', 'CARRY', 'PC_INC'),
    '(c->PC-1)&0xFFFF': ('PC', 'M1', 'CARRY', 'NONE'),
    '(c->PC-2)&0xFFFF': ('PC', 'M2', 'CARRY', 'NONE'),
    # the internal address register
    'c->AD': ('AD', '0', 'CARRY', 'NONE'),
    'c->AD++': ('AD', '0', 'CARRY', 'AD_INC'),
    'c->AD+c->X': ('AD', 'X', 'CARRY', 'NONE'),
    'c->AD+c->Y': ('AD', 'Y', 'CARRY', 'NONE'),
    '(c->AD+1)&0xFFFF': ('AD', '1', 'CARRY', 'NONE'),
    # zero page: the high byte never carries
    '_GD()': ('GD', '0', 'ZP', 'NONE'),
    '(c->AD+1)&0xFF': ('AD', '1', 'ZP', 'NONE'),
    '(c->AD+c->X)&0x00FF': ('AD', 'X', 'ZP', 'NONE'),
    '(c->AD+c->Y)&0x00FF': ('AD', 'Y', 'ZP', 'NONE'),
    # absolute, assembled from the byte just read and the one latched before it
    '(_GD()<<8)|c->AD': ('AD', '0', 'GD', 'NONE'),
    # stack
    '0x0100|c->S': ('S', '0', 'STACK', 'NONE'),
    '0x0100|c->S++': ('S', '0', 'STACK', 'S_INC'),
    '0x0100|c->S--': ('S', '0', 'STACK', 'S_DEC'),
    # a page held while the other half is recomputed: branch before fixup, and
    # the JMP (abs) pointer that wraps inside its own page
    '(c->PC&0xFF00)|(c->AD&0x00FF)': ('AD', '0', 'PC', 'NONE'),
    '(c->AD&0xFF00)|((c->AD+1)&0x00FF)': ('AD', '1', 'AD', 'NONE'),
    # ADC/SBC immediate spend their decimal cycle on a fixed internal address
    '0x007F': ('ZERO', '0', 'ZERO', 'NONE'),
    '0x0000': ('ZERO', '0', 'ZERO', 'NONE'),
    # the bus holds what the previous cycle drove
    '_GA()': ('HOLD', '0', 'HOLD', 'NONE'),
}

DATA_MAP = {
    'c->A': 'A',
    'c->X': 'X',
    'c->Y': 'Y',
    'c->AD': 'AD',
    '0x00': 'ZERO',
    'c->PC': 'PCL',
    'c->PC>>8': 'PCH',
    'c->P|W65C02_BF|W65C02_XF': 'P_BRK',
    'c->P|W65C02_XF|((c->brk_flags&(W65C02_BRK_IRQ|W65C02_BRK_NMI))?0:W65C02_BF)': 'P_ENTRY',
}


def check_vocabulary(gen):
    """Every address and data expression the C emits must be one we model."""
    for name, (lo, off, hi, post) in ADDR_MAP.items():
        assert lo in LO and off in OFF and hi in HI and post in POST, name

    missing = []
    for op, (mnem, am), ts in opcodes(gen):
        for i, t in enumerate(ts):
            for a in t.addrs:
                if a not in ADDR_MAP:
                    missing.append('$%02X %s t%d address: %s' % (op, mnem, i, a))
            for d in t.data_out:
                if d not in DATA_MAP:
                    missing.append('$%02X %s t%d data: %s' % (op, mnem, i, d))
    return missing


def report(gen):
    from collections import Counter

    ticks = Counter()
    addrs = Counter()
    douts = Counter()
    nwrite = nhold = nskip = ncond = nfetch = 0
    total = 0

    for op, (mnem, am), ts in opcodes(gen):
        ticks[len(ts)] += 1
        for t in ts:
            total += 1
            for a in t.addrs:
                addrs[a] += 1
            for d in t.data_out:
                douts[d] += 1
            nwrite += t.write
            nhold += t.holds_addr
            nskip += t.skip
            ncond += t.conditional
            nfetch += t.fetch

    print('opcodes 256, ticks %d' % total)
    print('ticks per opcode: %s' % dict(sorted(ticks.items())))
    print('write %d, address-hold %d, skip-next %d, conditional %d, fetch %d'
          % (nwrite, nhold, nskip, ncond, nfetch))

    print('\naddress sources (%d distinct)' % len(addrs))
    for a, n in addrs.most_common():
        print('  %4d  %s' % (n, a))

    print('\ndata-out sources (%d distinct)' % len(douts))
    for d, n in douts.most_common():
        print('  %4d  %s' % (n, d))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--report', action='store_true')
    args = ap.parse_args()

    gen = load_gen()

    missing = check_vocabulary(gen)
    if missing:
        sys.stderr.write('unmodelled expressions (%d):\n  %s\n'
                         % (len(missing), '\n  '.join(missing)))
        return 1

    if args.report:
        report(gen)
        return 0
    sys.stderr.write('nothing to do; try --report\n')
    return 1


if __name__ == '__main__':
    sys.exit(main())
