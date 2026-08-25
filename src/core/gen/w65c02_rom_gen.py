#!/usr/bin/env python3
#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Generate w65c02's decode tables from the same source as the C emulation.
#
# The decode table, addressing modes and per-opcode cycle sequences are read
# from vendor/chips/codegen/w65c02_gen.py rather than retyped. The RTL then
# cannot disagree with the emulator about which opcode takes how many cycles,
# and per-cycle lockstep is left to catch only what the two genuinely implement
# differently.
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
CODEGEN = os.path.join(ROOT, 'vendor', 'chips', 'codegen')


def load_gen():
    """The vendored generator, loaded by path so its name cannot collide."""
    sys.path.insert(0, CODEGEN)  # for the generator's own imports
    spec = importlib.util.spec_from_file_location(
        'w65c02_gen_vendored', os.path.join(CODEGEN, 'w65c02_gen.py'))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


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


def strip_bus(src):
    """What the cycle does to registers, with the bus statements removed."""
    for name in ('_SAD', '_SA', '_SD'):
        for a in macro_args(src, name):
            src = src.replace('%s(%s);' % (name, a), '')
            src = src.replace('%s(%s)' % (name, a), '')
    for tok in ('_WR();', '_FETCH();', 'c->IR++;', 'c->IR++'):
        src = src.replace(tok, '')
    # collapse the shells left behind by removing the only statement in a branch
    src = re.sub(r'\{\s*\}', '', src)
    src = re.sub(r'else\s*(?=[};]|$)', '', src)
    src = re.sub(r'\s+', '', src)
    return src.strip(';')


# Most of the operation tail is one shape repeated with a different register,
# bit or flag — the things a microword carries as a field rather than a case.
# Folding them shows how few distinct operations the datapath actually needs.
FOLD = (
    (r'>>\d\)&1', '>>BIT)&1'),                              # BBR/BBS bit select
    (r'&=~0x[0-9A-F]{2}', '&=~BIT'),                        # RMB
    (r'\|=0x[0-9A-F]{2}', '|=BIT'),                         # SMB
    (r'_w65c02_(adc|sbc)\(', r'_w65c02_ADDSUB('),
    (r'_w65c02_(asl|lsr|rol|ror)\(', r'_w65c02_SHIFT('),
    (r'_w65c02_cmp\(c,c->[AXY],', '_w65c02_cmp(c,REG,'),
    (r'c->P&W65C02_[NVCZ]F\)!=(?:0x[0-9A-F]+|0)', 'c->P&FLAG)!=VAL'),
    (r'c->([AXY])=_GD\(\);_NZ\(c->\1\)', 'c->REG=_GD();_NZ(c->REG)'),
    (r'c->A(\||&|\^)=_GD\(\);_NZ\(c->A\)', r'c->A LOGIC=_GD();_NZ(c->A)'),
    (r'c->(P&=~|P\|=)W65C02_[CID]F', r'c->\1W65C02_FLAG'),
    (r'c->A(\+\+|--);_NZ\(c->A\)', r'c->A STEP;_NZ(c->A)'),
    (r'c->AD(\+\+|--);_NZ\(c->AD\)', r'c->AD STEP;_NZ(c->AD)'),
    (r'\(\(\(c->AD\)\^\(c->AD\+c->[XY]\)\)&0xFF00\)', '((c->AD)^(c->AD+IDX))&0xFF00'),
)


def fold_op(s):
    for pat, rep in FOLD:
        s = re.sub(pat, rep, s)
    return s


class Tick:
    """The observable shape of one cycle, read out of the emitted C."""

    def __init__(self, src):
        self.src = src
        self.op = strip_bus(src)
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
        # Statement order: a few cycles read through AD while replacing it
        # (the BRK vector, JMP (abs,X), the zero-page pointer high bytes), so
        # their address uses the value from before this cycle's operation.
        sa = min((src.find(m) for m in ('_SA(', '_SAD(') if m in src),
                 default=-1)
        ld = src.find('c->AD=_GD()')
        self.ad_pre = (sa >= 0 and ld >= 0 and sa < ld
                       and any('c->AD' in a for a in self.addrs))

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
LO = ('PC', 'AD', 'S', 'GD', 'ZERO', 'CONST_7F', 'HOLD')
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
    # ADC/SBC immediate have no effective address, so their decimal cycle
    # reads whatever the part drives internally. Two distinct constants: the
    # vectors show $007F for ADC and $0000 for SBC.
    '0x007F': ('CONST_7F', '0', 'ZERO', 'NONE'),
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


# What the datapath does with registers, named. As with ADDR_MAP, every folded
# operation the C emits must appear here or generation fails, so an upstream
# change cannot reach the RTL unnoticed. Register, bit and flag selects are
# fields decoded from the opcode, not separate operations.
OP_MAP = {
    '': 'NONE',
    # moving bytes around
    'c->AD=_GD()': 'AD_LOAD',
    'c->AD|=_GD()<<8': 'AD_HI',
    'c->AD|=_GD()<<8;if(((c->AD)^(c->AD+IDX))&0xFF00==0)': 'AD_HI_INDEX',
    'c->AD=(c->AD+c->X)&0xFF': 'AD_ADD_X_ZP',
    'c->AD=(c->AD+c->X)&0xFFFF': 'AD_ADD_X',
    'c->REG=_GD();_NZ(c->REG)': 'LOAD',
    'c->PC=c->AD': 'PC_FROM_AD',
    'c->PC=(_GD()<<8)|c->AD': 'PC_FROM_ABS',
    'c->PC=_GD()': 'PCL_FROM_BUS',
    'c->PC=(_GD()<<8)|(c->PC&0xFF)': 'PCH_FROM_BUS',
    # arithmetic and logic
    'c->A LOGIC=_GD();_NZ(c->A)': 'LOGIC',
    '_w65c02_cmp(c,REG,_GD())': 'CMP',
    '_w65c02_bit(c,_GD())': 'BIT',
    'c->P&=~W65C02_ZF;if(0==(c->A&_GD())){c->P|=W65C02_ZF;}': 'BIT_IMM',
    'c->AD=_GD();if(0==(c->P&W65C02_DF)){_w65c02_ADDSUB(c,c->AD);}': 'ADDSUB_BINARY',
    '_w65c02_ADDSUB(c,c->AD)': 'ADDSUB_DECIMAL',
    'c->AD=_w65c02_SHIFT(c,c->AD)': 'SHIFT_AD',
    'c->A=_w65c02_SHIFT(c,c->A)': 'SHIFT_A',
    'c->AD STEP;_NZ(c->AD)': 'AD_STEP',
    'c->A STEP;_NZ(c->A)': 'A_STEP',
    'c->X++;_NZ(c->X)': 'X_INC',
    'c->X--;_NZ(c->X)': 'X_DEC',
    'c->Y++;_NZ(c->Y)': 'Y_INC',
    'c->Y--;_NZ(c->Y)': 'Y_DEC',
    # the bit instructions
    'c->AD=(c->AD>>BIT)&1': 'BIT_SELECT',
    'c->AD&=~BIT': 'RMB',
    'c->AD|=BIT': 'SMB',
    'c->P&=~W65C02_ZF;if(0==(c->A&c->AD)){c->P|=W65C02_ZF;}c->AD|=c->A': 'TSB',
    'c->P&=~W65C02_ZF;if(0==(c->A&c->AD)){c->P|=W65C02_ZF;}c->AD&=~c->A': 'TRB',
    # transfers
    'c->X=c->A;_NZ(c->X)': 'TAX',
    'c->Y=c->A;_NZ(c->Y)': 'TAY',
    'c->A=c->X;_NZ(c->A)': 'TXA',
    'c->A=c->Y;_NZ(c->A)': 'TYA',
    'c->X=c->S;_NZ(c->X)': 'TSX',
    'c->S=c->X': 'TXS',
    # flags
    'c->P&=~W65C02_FLAG': 'FLAG_CLEAR',
    'c->P|=W65C02_FLAG': 'FLAG_SET',
    'c->P&=~W65C02_VF': 'CLV',
    'c->P=(_GD()|W65C02_XF)&~W65C02_BF': 'PULL_P',
    # branches: compute the target, then fix the page up if it crossed
    'c->AD=c->PC+(int8_t)_GD();if((c->P&FLAG)!=VAL)': 'BRANCH_TEST',
    'c->AD=c->PC+(int8_t)_GD()': 'BRANCH_ALWAYS',
    'if((c->AD&0xFF00)==(c->PC&0xFF00)){c->PC=c->AD;}': 'BRANCH_FIXUP',
    'if((c->AD&0xFF00)==(c->PC&0xFF00)){c->PC=c->AD;c->irq_pip>>=1;c->nmi_pip>>=1;}':
        'BRANCH_FIXUP_PIP',
    'if(0==(uint8_t)c->AD){c->AD=c->PC+(int8_t)_GD();}': 'BBR_TEST',
    'if(0!=(uint8_t)c->AD){c->AD=c->PC+(int8_t)_GD();}': 'BBS_TEST',
    # interrupt entry and the stalls
    'if(0==(c->brk_flags&(W65C02_BRK_IRQ|W65C02_BRK_NMI))){c->PC++;}'
    'if(0==(c->brk_flags&W65C02_BRK_RESET))': 'BRK_PUSH_PCH',
    'if(0==(c->brk_flags&W65C02_BRK_RESET))': 'BRK_PUSH',
    'if(c->brk_flags&W65C02_BRK_RESET){c->AD=0xFFFC;}else{if(c->brk_flags&W65C02_BRK_NMI)'
    '{c->AD=0xFFFA;}else{c->AD=0xFFFE;}}': 'BRK_VECTOR',
    'c->P|=W65C02_FLAG;c->P&=~W65C02_FLAG;c->brk_flags=0': 'BRK_ENTER',
    'c->wait_flag=1': 'WAI',
    'c->stop_flag=1': 'STP',
}


# Which register, logic function, shift, flag, branch sense or bit an operation
# acts on. The C bakes these into each opcode's statement; the microword carries
# them as a select so the datapath has one implementation of each operation.
# Meaning depends on the op: SEL_A/X/Y for LOAD and CMP, SEL_OR/AND/EOR for
# LOGIC, and so on. Branches encode flag and sense; the bit ops their bit index.
SEL = ('NONE', 'A', 'X', 'Y', 'OR', 'AND', 'EOR', 'ADC', 'SBC',
       'ASL', 'LSR', 'ROL', 'ROR', 'C', 'I', 'D', 'INC', 'DEC',
       'B0', 'B1', 'B2', 'B3', 'B4', 'B5', 'B6', 'B7',
       'N_CLR', 'N_SET', 'V_CLR', 'V_SET', 'C_CLR', 'C_SET', 'Z_CLR', 'Z_SET')

SEL_RULES = (
    # (ops the rule serves, regex over the raw statement, sel from the match)
    (('LOAD',), r'c->([AXY])=_GD\(\)', lambda m: m.group(1)),
    (('CMP',), r'_w65c02_cmp\(c,c->([AXY]),', lambda m: m.group(1)),
    (('LOGIC',), r'c->A([|&^])=', lambda m: {'|': 'OR', '&': 'AND', '^': 'EOR'}[m.group(1)]),
    (('ADDSUB_BINARY', 'ADDSUB_DECIMAL'), r'_w65c02_(adc|sbc)\(',
     lambda m: m.group(1).upper()),
    (('SHIFT_A', 'SHIFT_AD'), r'_w65c02_(asl|lsr|rol|ror)\(',
     lambda m: m.group(1).upper()),
    (('FLAG_CLEAR', 'FLAG_SET'), r'c->P(?:&=~|\|=)W65C02_([CID])F',
     lambda m: m.group(1)),
    (('A_STEP', 'AD_STEP'), r'c->AD?(\+\+|--)',
     lambda m: 'INC' if m.group(1) == '++' else 'DEC'),
    (('BRANCH_TEST',), r'\(c->P&W65C02_([NVCZ])F\)!=(0x[0-9A-F]+|0)\b',
     lambda m: '%s_%s' % (m.group(1), 'CLR' if m.group(2) == '0' else 'SET')),
    (('BIT_SELECT',), r'>>(\d)\)&1', lambda m: 'B' + m.group(1)),
    (('RMB',), r'&=~0x([0-9A-F]{2})',
     lambda m: 'B%d' % (int(m.group(1), 16).bit_length() - 1)),
    (('SMB',), r'\|=0x([0-9A-F]{2})',
     lambda m: 'B%d' % (int(m.group(1), 16).bit_length() - 1)),
)


def tick_sel(opname, raw):
    for ops, pat, fn in SEL_RULES:
        if opname in ops:
            m = re.search(pat, raw)
            assert m, '%s did not match its own rule: %s' % (opname, raw)
            sel = fn(m)
            assert sel in SEL, sel
            return sel
    return 'NONE'


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
            if fold_op(t.op) not in OP_MAP:
                missing.append('$%02X %s t%d operation: %s'
                               % (op, mnem, i, fold_op(t.op)))

    # And nothing modelled that the C no longer emits, so the maps stay a
    # description of this instruction set rather than of its history.
    seen_a, seen_d, seen_o = set(), set(), set()
    for _, _, ts in opcodes(gen):
        for t in ts:
            seen_a.update(t.addrs)
            seen_d.update(t.data_out)
            seen_o.add(fold_op(t.op))
    for name, seen in (('address', set(ADDR_MAP) - seen_a),
                       ('data', set(DATA_MAP) - seen_d),
                       ('operation', set(OP_MAP) - seen_o)):
        for extra in sorted(seen):
            missing.append('unused %s mapping: %s' % (name, extra))

    return missing


def report(gen):
    from collections import Counter

    ticks = Counter()
    addrs = Counter()
    douts = Counter()
    nwrite = nhold = nskip = ncond = nfetch = 0
    total = 0

    for _, _, ts in opcodes(gen):
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

    ops, folded = Counter(), Counter()
    for _, _, ts in opcodes(gen):
        for t in ts:
            ops[t.op] += 1
            folded[fold_op(t.op)] += 1
    print('\noperations: %d distinct, %d after folding register, bit and flag '
          'selects' % (len(ops), len(folded)))
    for o, n in folded.most_common():
        print('  %4d  %s' % (n, o if o else '(none)'))


def enum_decl(name, values, width):
    body = ',\n'.join('        %s_%s' % (name.upper(), v) for v in values)
    return ('    typedef enum logic [%d:0] {\n%s\n    } w65c02_%s_t;\n'
            % (width - 1, body, name))


def bits(n):
    w = 1
    while (1 << w) < n:
        w += 1
    return w


def emit(gen, out):
    """The decode table as SystemVerilog, one entry per opcode and cycle.

    Written as a case rather than an array so it synthesizes to logic: the FPGA
    has no M10K blocks to spare for a CPU, and the machine's video and audio
    paths need every one of them.
    """
    ops = sorted(set(OP_MAP.values()))
    douts = sorted(set(DATA_MAP.values())) + ['NONE']

    lines = []
    lines.append('/*\n * Copyright (c) 2026 Rumbledethumps\n *\n'
                 ' * SPDX-License-Identifier: BSD-3-Clause\n *\n'
                 ' * Generated by src/core/gen/w65c02_rom_gen.py. Do not edit.\n'
                 ' *\n * The W65C02S decode table, taken from the same generator that emits\n'
                 ' * the C emulation, so the two cannot disagree about what any opcode does\n'
                 ' * or how long it takes.\n'
                 ' *\n'
                 ' * Some cycles decide something. Where an operation carries a condition,\n'
                 ' * an indexed address that crosses a page, a branch, a decimal add, the\n'
                 ' * x-prefixed address is the one to drive when the condition fails, and\n'
                 ' * fetch and skip apply only when it holds. Which cycles those are is the\n'
                 ' * operation, not a separate field.\n */\n')
    lines.append('package w65c02_rom_pkg;\n')
    lines.append(enum_decl('lo', LO, bits(len(LO))))
    lines.append(enum_decl('off', OFF, bits(len(OFF))))
    lines.append(enum_decl('hi', HI, bits(len(HI))))
    lines.append(enum_decl('post', POST, bits(len(POST))))
    lines.append(enum_decl('dout', douts, bits(len(douts))))
    lines.append(enum_decl('op', ops, bits(len(ops))))
    lines.append(enum_decl('sel', SEL, bits(len(SEL))))

    lines.append('''
    typedef struct packed {
        w65c02_lo_t   lo;
        w65c02_off_t  off;
        w65c02_hi_t   hi;
        w65c02_post_t post;
        // Taken instead when an indexed address carries into the next page.
        w65c02_lo_t   xlo;
        w65c02_off_t  xoff;
        w65c02_hi_t   xhi;
        w65c02_post_t xpost;
        w65c02_dout_t dout;
        w65c02_op_t   op;
        // Which register, function, flag, sense or bit the operation acts on.
        w65c02_sel_t  sel;
        // The address reads through AD from before this cycle's operation.
        logic         ad_pre;
        logic         wr;
        // Gated by the operation's condition where it has one.
        logic         fetch;
        logic         skip;
    } w65c02_cw_t;
''')

    def addr_fields(a):
        lo, off, hi, post = ADDR_MAP[a]
        return ('LO_%s, OFF_%s, HI_%s, POST_%s' % (lo, off, hi, post))

    hold = 'LO_HOLD, OFF_0, HI_HOLD, POST_NONE'

    lines.append('    function automatic w65c02_cw_t w65c02_rom'
                 '(input logic [10:0] ua);\n        case (ua)\n')
    for op, (mnem, am), ts in opcodes(gen):
        lines.append('        // $%02X %s %s\n' % (op, mnem, am))
        for t, tick in enumerate(ts):
            a = tick.addrs[0] if tick.addrs else None
            x = tick.addrs[1] if len(tick.addrs) > 1 else None
            opname = OP_MAP[fold_op(tick.op)]
            # Two addresses appear only where an indexed read decides between
            # the target and a dummy; its index register rides in the primary.
            if x is not None:
                assert opname == 'AD_HI_INDEX', tick.src
                assert ADDR_MAP[a][1] in ('X', 'Y'), tick.src
            prim = addr_fields(a) if a else hold
            alt = addr_fields(x) if x else prim
            dout = DATA_MAP[tick.data_out[0]] if tick.data_out else 'NONE'
            lines.append(
                "        11'h%03X: return '{%s, %s, DOUT_%s, OP_%s, SEL_%s, "
                "1'b%d, 1'b%d, 1'b%d, 1'b%d};\n"
                % ((op << 3) | t, prim, alt, dout, opname,
                   tick_sel(opname, tick.op), tick.ad_pre,
                   tick.write, tick.fetch, tick.skip))
    lines.append("        default: return '{%s, %s, DOUT_NONE, OP_NONE, SEL_NONE, "
                 "1'b0, 1'b0, 1'b0, 1'b0};\n" % (hold, hold))
    lines.append('        endcase\n    endfunction\n\nendpackage\n')

    with open(out, 'w') as f:
        f.write(''.join(lines))
    return sum(len(ts) for _, _, ts in opcodes(gen))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--emit', metavar='FILE')
    args = ap.parse_args()

    gen = load_gen()

    missing = check_vocabulary(gen)
    if missing:
        sys.stderr.write('unmodelled expressions (%d):\n  %s\n'
                         % (len(missing), '\n  '.join(missing)))
        return 1

    if args.emit:
        n = emit(gen, args.emit)
        print('%s: %d cycles over 256 opcodes' % (os.path.basename(args.emit), n))
        return 0
    if args.report:
        report(gen)
        return 0
    sys.stderr.write('nothing to do; try --report or --emit\n')
    return 1


if __name__ == '__main__':
    sys.exit(main())
