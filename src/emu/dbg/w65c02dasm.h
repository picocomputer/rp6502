#pragma once
/*#
    # w65c02dasm.h  --  WDC 65C02 (W65C02S) disassembler for the RP6502

    SPDX-License-Identifier: Zlib

    65C02 modifications Copyright (c) 2026 Rumbledethumps. This file is a
    derivative of floooh's zlib-licensed chips/util/m6502dasm.h (Copyright (c)
    2018 Andre Weissflog, full license retained below) and, per the zlib terms,
    is itself distributed under that same zlib license. Altered source is plainly
    marked as such here.

    The NMOS bit-pattern decoder was replaced with a flat 256-entry table that
    covers the full documented W65C02S instruction set: the CMOS additions (BRA,
    PHX/PLX/PHY/PLY, STZ, INC/DEC A, (zp) indirect, BIT #/zp,X/abs,X, TSB/TRB,
    JMP (abs,X), RMB/SMB/BBR/BBS, WAI, STP) and the documented multi-byte NOP
    fills. The PUBLIC API is kept identical to chips/util/m6502dasm.h -- the
    types m6502dasm_input_t / m6502dasm_output_t and the function m6502dasm_op --
    so ui_dbg.h (which calls m6502dasm_op when UI_DBG_USE_M6502 is defined)
    integrates unchanged. Include it before ui_dbg.h, in exactly one translation
    unit with CHIPS_UTIL_IMPL defined, and do NOT include it alongside
    chips/util/m6502dasm.h.

    ## zlib/libpng license

    Copyright (c) 2018 Andre Weissflog
    This software is provided 'as-is', without any express or implied warranty.
    In no event will the authors be held liable for any damages arising from the
    use of this software.
    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:
        1. The origin of this software must not be misrepresented; you must not
        claim that you wrote the original software. If you use this software in a
        product, an acknowledgment in the product documentation would be
        appreciated but is not required.
        2. Altered source versions must be plainly marked as such, and must not
        be misrepresented as being the original software.
        3. This notice may not be removed or altered from any source
        distribution.
#*/
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the input callback type */
typedef uint8_t (*m6502dasm_input_t)(void* user_data);
/* the output callback type */
typedef void (*m6502dasm_output_t)(char c, void* user_data);

/* disassemble a single W65C02S instruction into a stream of ASCII characters */
uint16_t m6502dasm_op(uint16_t pc, m6502dasm_input_t in_cb, m6502dasm_output_t out_cb, void* user_data);

#ifdef __cplusplus
} /* extern "C" */
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_UTIL_IMPL
#ifndef CHIPS_ASSERT
    #include <assert.h>
    #define CHIPS_ASSERT(c) assert(c)
#endif

/* fetch unsigned 8-bit value and track pc */
#define _FETCH_U8(v) v=in_cb(user_data);pc++;
/* fetch signed 8-bit value and track pc */
#define _FETCH_I8(v) v=(int8_t)in_cb(user_data);pc++;
/* fetch unsigned 16-bit value and track pc */
#define _FETCH_U16(v) v=in_cb(user_data);v|=in_cb(user_data)<<8;pc+=2;
/* output character */
#define _CHR(c) if (out_cb) { out_cb(c,user_data); }
/* output string */
#define _STR(s) _w65c02dasm_str(s,out_cb,user_data);
/* output number as unsigned 8-bit hex string */
#define _STR_U8(u8) _w65c02dasm_u8((uint8_t)(u8),out_cb,user_data);
/* output number as unsigned 16-bit hex string */
#define _STR_U16(u16) _w65c02dasm_u16((uint16_t)(u16),out_cb,user_data);

/* addressing modes */
enum {
    AM_IMP,  /* implied / stack            */
    AM_ACC,  /* accumulator      A         */
    AM_IMM,  /* immediate        #$nn      */
    AM_ZP,   /* zero page        $nn       */
    AM_ZPX,  /* zero page,X      $nn,X     */
    AM_ZPY,  /* zero page,Y      $nn,Y     */
    AM_IZX,  /* (zp,X)          ($nn,X)    */
    AM_IZY,  /* (zp),Y          ($nn),Y    */
    AM_IZP,  /* (zp)            ($nn)      [65C02] */
    AM_ABS,  /* absolute         $nnnn     */
    AM_ABX,  /* absolute,X       $nnnn,X   */
    AM_ABY,  /* absolute,Y       $nnnn,Y   */
    AM_IND,  /* (abs)           ($nnnn)    */
    AM_IAX,  /* (abs,X)         ($nnnn,X)  [65C02] */
    AM_REL,  /* relative branch  $nnnn     */
    AM_ZPR,  /* zp, relative     $nn,$nnnn [65C02 BBR/BBS] */
};

typedef struct { const char* mnem; uint8_t mode; } _w65c02dasm_op_t;

/* full W65C02S opcode table (documented instruction set; the "unused" opcodes
 * are the documented NOP fills of the appropriate length) */
static const _w65c02dasm_op_t _w65c02dasm_ops[256] = {
/* 0x */ {"BRK",AM_IMP},{"ORA",AM_IZX},{"NOP",AM_IMM},{"NOP",AM_IMP},{"TSB",AM_ZP },{"ORA",AM_ZP },{"ASL",AM_ZP },{"RMB0",AM_ZP},{"PHP",AM_IMP},{"ORA",AM_IMM},{"ASL",AM_ACC},{"NOP",AM_IMP},{"TSB",AM_ABS},{"ORA",AM_ABS},{"ASL",AM_ABS},{"BBR0",AM_ZPR},
/* 1x */ {"BPL",AM_REL},{"ORA",AM_IZY},{"ORA",AM_IZP},{"NOP",AM_IMP},{"TRB",AM_ZP },{"ORA",AM_ZPX},{"ASL",AM_ZPX},{"RMB1",AM_ZP},{"CLC",AM_IMP},{"ORA",AM_ABY},{"INC",AM_ACC},{"NOP",AM_IMP},{"TRB",AM_ABS},{"ORA",AM_ABX},{"ASL",AM_ABX},{"BBR1",AM_ZPR},
/* 2x */ {"JSR",AM_ABS},{"AND",AM_IZX},{"NOP",AM_IMM},{"NOP",AM_IMP},{"BIT",AM_ZP },{"AND",AM_ZP },{"ROL",AM_ZP },{"RMB2",AM_ZP},{"PLP",AM_IMP},{"AND",AM_IMM},{"ROL",AM_ACC},{"NOP",AM_IMP},{"BIT",AM_ABS},{"AND",AM_ABS},{"ROL",AM_ABS},{"BBR2",AM_ZPR},
/* 3x */ {"BMI",AM_REL},{"AND",AM_IZY},{"AND",AM_IZP},{"NOP",AM_IMP},{"BIT",AM_ZPX},{"AND",AM_ZPX},{"ROL",AM_ZPX},{"RMB3",AM_ZP},{"SEC",AM_IMP},{"AND",AM_ABY},{"DEC",AM_ACC},{"NOP",AM_IMP},{"BIT",AM_ABX},{"AND",AM_ABX},{"ROL",AM_ABX},{"BBR3",AM_ZPR},
/* 4x */ {"RTI",AM_IMP},{"EOR",AM_IZX},{"NOP",AM_IMM},{"NOP",AM_IMP},{"NOP",AM_ZP },{"EOR",AM_ZP },{"LSR",AM_ZP },{"RMB4",AM_ZP},{"PHA",AM_IMP},{"EOR",AM_IMM},{"LSR",AM_ACC},{"NOP",AM_IMP},{"JMP",AM_ABS},{"EOR",AM_ABS},{"LSR",AM_ABS},{"BBR4",AM_ZPR},
/* 5x */ {"BVC",AM_REL},{"EOR",AM_IZY},{"EOR",AM_IZP},{"NOP",AM_IMP},{"NOP",AM_ZPX},{"EOR",AM_ZPX},{"LSR",AM_ZPX},{"RMB5",AM_ZP},{"CLI",AM_IMP},{"EOR",AM_ABY},{"PHY",AM_IMP},{"NOP",AM_IMP},{"NOP",AM_ABS},{"EOR",AM_ABX},{"LSR",AM_ABX},{"BBR5",AM_ZPR},
/* 6x */ {"RTS",AM_IMP},{"ADC",AM_IZX},{"NOP",AM_IMM},{"NOP",AM_IMP},{"STZ",AM_ZP },{"ADC",AM_ZP },{"ROR",AM_ZP },{"RMB6",AM_ZP},{"PLA",AM_IMP},{"ADC",AM_IMM},{"ROR",AM_ACC},{"NOP",AM_IMP},{"JMP",AM_IND},{"ADC",AM_ABS},{"ROR",AM_ABS},{"BBR6",AM_ZPR},
/* 7x */ {"BVS",AM_REL},{"ADC",AM_IZY},{"ADC",AM_IZP},{"NOP",AM_IMP},{"STZ",AM_ZPX},{"ADC",AM_ZPX},{"ROR",AM_ZPX},{"RMB7",AM_ZP},{"SEI",AM_IMP},{"ADC",AM_ABY},{"PLY",AM_IMP},{"NOP",AM_IMP},{"JMP",AM_IAX},{"ADC",AM_ABX},{"ROR",AM_ABX},{"BBR7",AM_ZPR},
/* 8x */ {"BRA",AM_REL},{"STA",AM_IZX},{"NOP",AM_IMM},{"NOP",AM_IMP},{"STY",AM_ZP },{"STA",AM_ZP },{"STX",AM_ZP },{"SMB0",AM_ZP},{"DEY",AM_IMP},{"BIT",AM_IMM},{"TXA",AM_IMP},{"NOP",AM_IMP},{"STY",AM_ABS},{"STA",AM_ABS},{"STX",AM_ABS},{"BBS0",AM_ZPR},
/* 9x */ {"BCC",AM_REL},{"STA",AM_IZY},{"STA",AM_IZP},{"NOP",AM_IMP},{"STY",AM_ZPX},{"STA",AM_ZPX},{"STX",AM_ZPY},{"SMB1",AM_ZP},{"TYA",AM_IMP},{"STA",AM_ABY},{"TXS",AM_IMP},{"NOP",AM_IMP},{"STZ",AM_ABS},{"STA",AM_ABX},{"STZ",AM_ABX},{"BBS1",AM_ZPR},
/* Ax */ {"LDY",AM_IMM},{"LDA",AM_IZX},{"LDX",AM_IMM},{"NOP",AM_IMP},{"LDY",AM_ZP },{"LDA",AM_ZP },{"LDX",AM_ZP },{"SMB2",AM_ZP},{"TAY",AM_IMP},{"LDA",AM_IMM},{"TAX",AM_IMP},{"NOP",AM_IMP},{"LDY",AM_ABS},{"LDA",AM_ABS},{"LDX",AM_ABS},{"BBS2",AM_ZPR},
/* Bx */ {"BCS",AM_REL},{"LDA",AM_IZY},{"LDA",AM_IZP},{"NOP",AM_IMP},{"LDY",AM_ZPX},{"LDA",AM_ZPX},{"LDX",AM_ZPY},{"SMB3",AM_ZP},{"CLV",AM_IMP},{"LDA",AM_ABY},{"TSX",AM_IMP},{"NOP",AM_IMP},{"LDY",AM_ABX},{"LDA",AM_ABX},{"LDX",AM_ABY},{"BBS3",AM_ZPR},
/* Cx */ {"CPY",AM_IMM},{"CMP",AM_IZX},{"NOP",AM_IMM},{"NOP",AM_IMP},{"CPY",AM_ZP },{"CMP",AM_ZP },{"DEC",AM_ZP },{"SMB4",AM_ZP},{"INY",AM_IMP},{"CMP",AM_IMM},{"DEX",AM_IMP},{"WAI",AM_IMP},{"CPY",AM_ABS},{"CMP",AM_ABS},{"DEC",AM_ABS},{"BBS4",AM_ZPR},
/* Dx */ {"BNE",AM_REL},{"CMP",AM_IZY},{"CMP",AM_IZP},{"NOP",AM_IMP},{"NOP",AM_ZPX},{"CMP",AM_ZPX},{"DEC",AM_ZPX},{"SMB5",AM_ZP},{"CLD",AM_IMP},{"CMP",AM_ABY},{"PHX",AM_IMP},{"STP",AM_IMP},{"NOP",AM_ABS},{"CMP",AM_ABX},{"DEC",AM_ABX},{"BBS5",AM_ZPR},
/* Ex */ {"CPX",AM_IMM},{"SBC",AM_IZX},{"NOP",AM_IMM},{"NOP",AM_IMP},{"CPX",AM_ZP },{"SBC",AM_ZP },{"INC",AM_ZP },{"SMB6",AM_ZP},{"INX",AM_IMP},{"SBC",AM_IMM},{"NOP",AM_IMP},{"NOP",AM_IMP},{"CPX",AM_ABS},{"SBC",AM_ABS},{"INC",AM_ABS},{"BBS6",AM_ZPR},
/* Fx */ {"BEQ",AM_REL},{"SBC",AM_IZY},{"SBC",AM_IZP},{"NOP",AM_IMP},{"NOP",AM_ZPX},{"SBC",AM_ZPX},{"INC",AM_ZPX},{"SMB7",AM_ZP},{"SED",AM_IMP},{"SBC",AM_ABY},{"PLX",AM_IMP},{"NOP",AM_IMP},{"NOP",AM_ABS},{"SBC",AM_ABX},{"INC",AM_ABX},{"BBS7",AM_ZPR},
};

static const char* _w65c02dasm_hex = "0123456789ABCDEF";

static void _w65c02dasm_str(const char* str, m6502dasm_output_t out_cb, void* user_data) {
    if (out_cb) {
        char c;
        while (0 != (c = *str++)) {
            out_cb(c, user_data);
        }
    }
}

static void _w65c02dasm_u8(uint8_t val, m6502dasm_output_t out_cb, void* user_data) {
    if (out_cb) {
        out_cb('$', user_data);
        for (int i = 1; i >= 0; i--) {
            out_cb(_w65c02dasm_hex[(val>>(i*4)) & 0xF], user_data);
        }
    }
}

static void _w65c02dasm_u16(uint16_t val, m6502dasm_output_t out_cb, void* user_data) {
    if (out_cb) {
        out_cb('$', user_data);
        for (int i = 3; i >= 0; i--) {
            out_cb(_w65c02dasm_hex[(val>>(i*4)) & 0xF], user_data);
        }
    }
}

/* main disassembler function */
uint16_t m6502dasm_op(uint16_t pc, m6502dasm_input_t in_cb, m6502dasm_output_t out_cb, void* user_data) {
    CHIPS_ASSERT(in_cb);
    uint8_t op;
    _FETCH_U8(op);
    const _w65c02dasm_op_t* e = &_w65c02dasm_ops[op];
    _STR(e->mnem);

    uint8_t u8; int8_t i8; uint16_t u16;
    switch (e->mode) {
        case AM_IMP:
            break;
        case AM_ACC:
            _STR(" A");
            break;
        case AM_IMM:
            _STR(" #"); _FETCH_U8(u8); _STR_U8(u8);
            break;
        case AM_ZP:
            _CHR(' '); _FETCH_U8(u8); _STR_U8(u8);
            break;
        case AM_ZPX:
            _CHR(' '); _FETCH_U8(u8); _STR_U8(u8); _STR(",X");
            break;
        case AM_ZPY:
            _CHR(' '); _FETCH_U8(u8); _STR_U8(u8); _STR(",Y");
            break;
        case AM_IZX:
            _CHR(' '); _FETCH_U8(u8); _CHR('('); _STR_U8(u8); _STR(",X)");
            break;
        case AM_IZY:
            _CHR(' '); _FETCH_U8(u8); _CHR('('); _STR_U8(u8); _STR("),Y");
            break;
        case AM_IZP:
            _CHR(' '); _FETCH_U8(u8); _CHR('('); _STR_U8(u8); _CHR(')');
            break;
        case AM_ABS:
            _CHR(' '); _FETCH_U16(u16); _STR_U16(u16);
            break;
        case AM_ABX:
            _CHR(' '); _FETCH_U16(u16); _STR_U16(u16); _STR(",X");
            break;
        case AM_ABY:
            _CHR(' '); _FETCH_U16(u16); _STR_U16(u16); _STR(",Y");
            break;
        case AM_IND:
            _CHR(' '); _FETCH_U16(u16); _CHR('('); _STR_U16(u16); _CHR(')');
            break;
        case AM_IAX:
            _CHR(' '); _FETCH_U16(u16); _CHR('('); _STR_U16(u16); _STR(",X)");
            break;
        case AM_REL:
            _CHR(' '); _FETCH_I8(i8); _STR_U16(pc + i8);
            break;
        case AM_ZPR: /* BBRx/BBSx: zero-page byte then a relative target */
            _CHR(' '); _FETCH_U8(u8); _STR_U8(u8); _CHR(',');
            _FETCH_I8(i8); _STR_U16(pc + i8);
            break;
    }
    return pc;
}

#undef _FETCH_I8
#undef _FETCH_U8
#undef _FETCH_U16
#undef _CHR
#undef _STR
#undef _STR_U8
#undef _STR_U16
#endif /* CHIPS_UTIL_IMPL */
