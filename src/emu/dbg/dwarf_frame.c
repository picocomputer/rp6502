/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * DWARF .debug_frame reader + unwinder — see dwarf_frame.h. Parses CIE/FDE
 * entries, runs the CFI rule program to build the row covering a PC, and
 * evaluates the CFA / register-recovery rules (including the small DWARF
 * expressions llvm-mos emits to normalize the 6502 hardware stack). Defensive
 * against truncated input: a bad entry is skipped; a bad rule fails the unwind.
 */

#include "emu/dbg/dwarf_frame.h"
#include "emu/dbg/dwarf_cursor.h"
#include "emu/dbg/dwarf_elf.h"

#include <stdlib.h>
#include <string.h>

/* MOS soft-stack pointer register (MOSRegisterInfo.td Imag16RegsOffset). */
#define DW_MOS_RS0 0x30000u

/* Call-frame instructions (only the ones we parse). */
enum
{
    CFA_advance_loc = 0x40, /* high 2 bits */
    CFA_offset = 0x80,
    CFA_restore = 0xc0,
    CFA_nop = 0x00,
    CFA_set_loc = 0x01,
    CFA_advance_loc1 = 0x02,
    CFA_advance_loc2 = 0x03,
    CFA_advance_loc4 = 0x04,
    CFA_offset_extended = 0x05,
    CFA_restore_extended = 0x06,
    CFA_undefined = 0x07,
    CFA_same_value = 0x08,
    CFA_register = 0x09,
    CFA_remember_state = 0x0a,
    CFA_restore_state = 0x0b,
    CFA_def_cfa = 0x0c,
    CFA_def_cfa_register = 0x0d,
    CFA_def_cfa_offset = 0x0e,
    CFA_def_cfa_expression = 0x0f,
    CFA_expression = 0x10,
    CFA_offset_extended_sf = 0x11,
    CFA_def_cfa_sf = 0x12,
    CFA_def_cfa_offset_sf = 0x13,
    CFA_val_offset = 0x14,
    CFA_val_offset_sf = 0x15,
    CFA_val_expression = 0x16,
};

/* DWARF expression opcodes used by the CFI expressions (fixed, tiny vocabulary). */
enum
{
    OP_deref = 0x06,
    OP_const1u = 0x08,
    OP_const1s = 0x09,
    OP_const2u = 0x0a,
    OP_const4u = 0x0c,
    OP_constu = 0x10,
    OP_consts = 0x11,
    OP_and = 0x1a,
    OP_minus = 0x1c,
    OP_or = 0x21,
    OP_plus = 0x22,
    OP_plus_uconst = 0x23,
    OP_lit0 = 0x30,
    OP_lit31 = 0x4f,
    OP_breg0 = 0x70,
    OP_breg31 = 0x8f,
    OP_bregx = 0x92,
};

typedef struct
{
    uint32_t off;              /* offset of this CIE within .debug_frame */
    int64_t data_align;
    uint32_t code_align;
    uint32_t ra_column;
    const uint8_t *insns;
    uint32_t insns_len;
} cie_t;

typedef struct
{
    uint32_t cie_off;
    uint32_t initial_loc;
    uint32_t range;
    const uint8_t *insns;
    uint32_t insns_len;
} fde_t;

struct dwarf_frame
{
    uint8_t *frame; /* a private copy of .debug_frame; insns point into it */
    cie_t *cies;
    int ncies;
    fde_t *fdes;
    int nfdes;
    uint8_t addr_size;
};

/* ---- CFI row state ---- */
enum
{
    RULE_UNDEF = 0,
    RULE_OFFSET,     /* value = *(CFA + off) */
    RULE_VAL_OFFSET, /* value = CFA + off */
    RULE_EXPR,       /* value = *(eval expr) */
    RULE_VAL_EXPR,   /* value = eval expr */
    RULE_REG,        /* value = other register */
    RULE_SAME,       /* value = current register */
};
typedef struct
{
    int type;
    int64_t off; /* already multiplied by data_align where applicable */
    const uint8_t *expr;
    uint32_t elen;
    int reg;
} rule_t;
typedef struct
{
    int cfa_is_expr;
    uint64_t cfa_reg;
    int64_t cfa_off; /* unfactored */
    const uint8_t *cfa_expr;
    uint32_t cfa_elen;
    rule_t ra, s, rs0; /* only the columns we recover */
} state_t;

/* Which tracked rule (if any) a register column maps to. */
static rule_t *slot_for(state_t *st, uint64_t reg, uint32_t ra_col)
{
    if (reg == ra_col)
        return &st->ra;
    if (reg == 4) /* S */
        return &st->s;
    if (reg == DW_MOS_RS0)
        return &st->rs0;
    return NULL; /* a column we don't need; parse but ignore its rule */
}

/* Push one value; overflow of the tiny fixed eval stack fails the expression. */
static void eval_push(uint32_t *st, int *sp, uint32_t v, bool *ok)
{
    if (*sp < 32) st[(*sp)++] = v; else *ok = false;
}

/* Evaluate a DWARF expression to a 32-bit value using the live registers. */
static uint32_t eval_expr(const uint8_t *e, uint32_t len, uint16_t s16, uint16_t rs0,
                          uint8_t (*rd)(uint16_t), bool *ok)
{
    uint32_t st[32];
    int sp = 0;
    dwarf_cur c = {e, e + len, true};
    while (c.p < c.end && c.ok && *ok)
    {
        uint8_t op = dwarf_u8(&c);
        if (op >= OP_breg0 && op <= OP_breg31)
        {
            int reg = op - OP_breg0;
            int64_t off = dwarf_sleb(&c);
            uint32_t rv = (reg == 4) ? s16 : 0;
            eval_push(st, &sp, (uint32_t)(rv + off), ok);
        }
        else if (op >= OP_lit0 && op <= OP_lit31)
        {
            eval_push(st, &sp, op - OP_lit0, ok);
        }
        else
        {
            switch (op)
            {
            case OP_const1u: eval_push(st, &sp, dwarf_u8(&c), ok); break;
            case OP_const1s: eval_push(st, &sp, (uint32_t)(int8_t)dwarf_u8(&c), ok); break;
            case OP_const2u: eval_push(st, &sp, dwarf_u16(&c), ok); break;
            case OP_const4u: eval_push(st, &sp, dwarf_u32(&c), ok); break;
            case OP_constu: eval_push(st, &sp, (uint32_t)dwarf_uleb(&c), ok); break;
            case OP_consts: eval_push(st, &sp, (uint32_t)dwarf_sleb(&c), ok); break;
            case OP_and: if (sp >= 2) { uint32_t b = st[--sp], a = st[--sp]; eval_push(st, &sp, a & b, ok); } else *ok = false; break;
            case OP_or: if (sp >= 2) { uint32_t b = st[--sp], a = st[--sp]; eval_push(st, &sp, a | b, ok); } else *ok = false; break;
            case OP_plus: if (sp >= 2) { uint32_t b = st[--sp], a = st[--sp]; eval_push(st, &sp, a + b, ok); } else *ok = false; break;
            case OP_minus: if (sp >= 2) { uint32_t b = st[--sp], a = st[--sp]; eval_push(st, &sp, a - b, ok); } else *ok = false; break;
            case OP_plus_uconst: if (sp >= 1) { uint32_t k = (uint32_t)dwarf_uleb(&c); st[sp - 1] += k; } else *ok = false; break;
            case OP_deref:
                if (sp >= 1) { uint16_t a = (uint16_t)st[sp - 1]; st[sp - 1] = (uint32_t)(rd(a) | (rd((uint16_t)(a + 1)) << 8)); }
                else *ok = false;
                break;
            case OP_bregx:
            {
                uint64_t reg = dwarf_uleb(&c);
                int64_t off = dwarf_sleb(&c);
                uint32_t rv = (reg == 4) ? s16 : (reg == DW_MOS_RS0 ? rs0 : 0);
                eval_push(st, &sp, (uint32_t)(rv + off), ok);
                break;
            }
            default:
                *ok = false;
                break;
            }
        }
    }
    if (!c.ok) *ok = false;
    return (sp > 0) ? st[sp - 1] : (*ok = false, 0);
}

/* Run a CFI instruction stream into `st`, advancing `*loc` and stopping once it
 * would pass `target`. `init` (may be NULL) is the CIE-initial state used by the
 * restore opcodes. Uses a small remember/restore stack. */
static void run_insns(const uint8_t *insns, uint32_t len, state_t *st, const state_t *init,
                      uint32_t code_align, int64_t data_align, uint32_t ra_col,
                      uint32_t addr_size, uint32_t *loc, uint32_t target)
{
    dwarf_cur c = {insns, insns + len, true};
    state_t stack[8];
    int depth = 0;
    while (c.p < c.end && c.ok)
    {
        uint8_t op = dwarf_u8(&c);
        uint8_t hi = op & 0xc0;
        if (hi == CFA_advance_loc)
        {
            uint32_t nl = *loc + (op & 0x3f) * code_align;
            if (nl > target) break;
            *loc = nl;
            continue;
        }
        if (hi == CFA_offset)
        {
            uint64_t reg = op & 0x3f;
            int64_t off = (int64_t)dwarf_uleb(&c) * data_align;
            rule_t *r = slot_for(st, reg, ra_col);
            if (r) { r->type = RULE_OFFSET; r->off = off; }
            continue;
        }
        if (hi == CFA_restore)
        {
            uint64_t reg = op & 0x3f;
            rule_t *r = slot_for(st, reg, ra_col);
            if (r && init)
            {
                if (reg == ra_col) *r = init->ra;
                else if (reg == 4) *r = init->s;
                else if (reg == DW_MOS_RS0) *r = init->rs0;
            }
            continue;
        }
        switch (op)
        {
        case CFA_nop:
            break;
        case CFA_set_loc:
        {
            uint32_t a = (addr_size == 8) ? (uint32_t)dwarf_u64(&c) : dwarf_u32(&c);
            if (a > target) { c.ok = false; } /* next row starts past pc; stop */
            else *loc = a;
            break;
        }
        case CFA_advance_loc1: { uint32_t nl = *loc + dwarf_u8(&c) * code_align; if (nl > target) { c.ok = false; break; } *loc = nl; break; }
        case CFA_advance_loc2: { uint32_t nl = *loc + dwarf_u16(&c) * code_align; if (nl > target) { c.ok = false; break; } *loc = nl; break; }
        case CFA_advance_loc4: { uint32_t nl = *loc + dwarf_u32(&c) * code_align; if (nl > target) { c.ok = false; break; } *loc = nl; break; }
        case CFA_offset_extended: { uint64_t reg = dwarf_uleb(&c); int64_t off = (int64_t)dwarf_uleb(&c) * data_align; rule_t *r = slot_for(st, reg, ra_col); if (r) { r->type = RULE_OFFSET; r->off = off; } break; }
        case CFA_offset_extended_sf: { uint64_t reg = dwarf_uleb(&c); int64_t off = dwarf_sleb(&c) * data_align; rule_t *r = slot_for(st, reg, ra_col); if (r) { r->type = RULE_OFFSET; r->off = off; } break; }
        case CFA_val_offset: { uint64_t reg = dwarf_uleb(&c); int64_t off = (int64_t)dwarf_uleb(&c) * data_align; rule_t *r = slot_for(st, reg, ra_col); if (r) { r->type = RULE_VAL_OFFSET; r->off = off; } break; }
        case CFA_val_offset_sf: { uint64_t reg = dwarf_uleb(&c); int64_t off = dwarf_sleb(&c) * data_align; rule_t *r = slot_for(st, reg, ra_col); if (r) { r->type = RULE_VAL_OFFSET; r->off = off; } break; }
        case CFA_restore_extended: { uint64_t reg = dwarf_uleb(&c); rule_t *r = slot_for(st, reg, ra_col); if (r && init) { if (reg == ra_col) *r = init->ra; else if (reg == 4) *r = init->s; else if (reg == DW_MOS_RS0) *r = init->rs0; } break; }
        case CFA_undefined: { uint64_t reg = dwarf_uleb(&c); rule_t *r = slot_for(st, reg, ra_col); if (r) { r->type = RULE_UNDEF; } break; }
        case CFA_same_value: { uint64_t reg = dwarf_uleb(&c); rule_t *r = slot_for(st, reg, ra_col); if (r) { r->type = RULE_SAME; } break; }
        case CFA_register: { uint64_t reg = dwarf_uleb(&c); uint64_t reg2 = dwarf_uleb(&c); rule_t *r = slot_for(st, reg, ra_col); if (r) { r->type = RULE_REG; r->reg = (int)reg2; } break; }
        case CFA_remember_state: if (depth < 8) stack[depth++] = *st; break;
        case CFA_restore_state: if (depth > 0) *st = stack[--depth]; break;
        case CFA_def_cfa: { st->cfa_is_expr = 0; st->cfa_reg = dwarf_uleb(&c); st->cfa_off = (int64_t)dwarf_uleb(&c); break; }
        case CFA_def_cfa_sf: { st->cfa_is_expr = 0; st->cfa_reg = dwarf_uleb(&c); st->cfa_off = dwarf_sleb(&c) * data_align; break; }
        case CFA_def_cfa_register: st->cfa_is_expr = 0; st->cfa_reg = dwarf_uleb(&c); break;
        case CFA_def_cfa_offset: st->cfa_is_expr = 0; st->cfa_off = (int64_t)dwarf_uleb(&c); break;
        case CFA_def_cfa_offset_sf: st->cfa_is_expr = 0; st->cfa_off = dwarf_sleb(&c) * data_align; break;
        case CFA_def_cfa_expression:
        {
            uint64_t n = dwarf_uleb(&c);
            if (n > (uint64_t)(c.end - c.p)) { c.ok = false; break; }
            st->cfa_is_expr = 1;
            st->cfa_expr = c.p;
            st->cfa_elen = (uint32_t)n;
            c.p += n;
            break;
        }
        case CFA_expression:
        case CFA_val_expression:
        {
            uint64_t reg = dwarf_uleb(&c);
            uint64_t n = dwarf_uleb(&c);
            if (n > (uint64_t)(c.end - c.p)) { c.ok = false; break; }
            rule_t *r = slot_for(st, reg, ra_col);
            if (r) { r->type = (op == CFA_expression) ? RULE_EXPR : RULE_VAL_EXPR; r->expr = c.p; r->elen = (uint32_t)n; }
            c.p += n;
            break;
        }
        default:
            c.ok = false; /* unknown opcode: can't size operands safely */
            break;
        }
    }
}

static const cie_t *find_cie(const dwarf_frame_t *df, uint32_t off)
{
    for (int i = 0; i < df->ncies; i++)
        if (df->cies[i].off == off)
            return &df->cies[i];
    return NULL;
}

static const fde_t *find_fde(const dwarf_frame_t *df, uint16_t pc)
{
    for (int i = 0; i < df->nfdes; i++)
        if (pc >= df->fdes[i].initial_loc && pc < df->fdes[i].initial_loc + df->fdes[i].range)
            return &df->fdes[i];
    return NULL;
}

/* Apply a recovered-register rule, given the frame's CFA and live regs. */
static bool apply_rule(const rule_t *r, uint32_t cfa, uint16_t s16, uint16_t rs0,
                       uint8_t (*rd)(uint16_t), uint16_t cur, uint16_t *out)
{
    bool ok = true;
    switch (r->type)
    {
    case RULE_VAL_OFFSET: *out = (uint16_t)(cfa + r->off); return true;
    case RULE_OFFSET: { uint16_t a = (uint16_t)(cfa + r->off); *out = (uint16_t)(rd(a) | (rd((uint16_t)(a + 1)) << 8)); return true; }
    case RULE_VAL_EXPR: *out = (uint16_t)eval_expr(r->expr, r->elen, s16, rs0, rd, &ok); return ok;
    case RULE_EXPR: { uint16_t a = (uint16_t)eval_expr(r->expr, r->elen, s16, rs0, rd, &ok); if (!ok) return false; *out = (uint16_t)(rd(a) | (rd((uint16_t)(a + 1)) << 8)); return true; }
    case RULE_REG:
        if (r->reg == 4) { *out = s16; return true; }
        if (r->reg == DW_MOS_RS0) { *out = rs0; return true; }
        return false; /* value held in a register we don't track */
    case RULE_SAME: *out = cur; return true;
    case RULE_UNDEF:
    default: return false;
    }
}

dwarf_unwind_t dwarf_frame_step(const dwarf_frame_t *df, uint16_t pc,
                                uint16_t s16, uint16_t rs0,
                                uint8_t (*readmem)(uint16_t))
{
    dwarf_unwind_t u;
    memset(&u, 0, sizeof u);
    if (!df || !readmem)
        return u;
    const fde_t *fde = find_fde(df, pc);
    if (!fde)
        return u;
    const cie_t *cie = find_cie(df, fde->cie_off);
    if (!cie)
        return u;

    /* CIE-initial row, then FDE rows up to pc. */
    state_t init;
    memset(&init, 0, sizeof init);
    uint32_t loc = fde->initial_loc;
    run_insns(cie->insns, cie->insns_len, &init, NULL,
              cie->code_align, cie->data_align, cie->ra_column, df->addr_size, &loc, 0xffffffffu);
    state_t st = init;
    loc = fde->initial_loc;
    run_insns(fde->insns, fde->insns_len, &st, &init,
              cie->code_align, cie->data_align, cie->ra_column, df->addr_size, &loc, pc);

    /* CFA. */
    bool ok = true;
    uint32_t cfa;
    if (st.cfa_is_expr)
        cfa = eval_expr(st.cfa_expr, st.cfa_elen, s16, rs0, readmem, &ok);
    else if (st.cfa_reg == 4)
        cfa = s16 + (uint32_t)st.cfa_off;
    else if (st.cfa_reg == DW_MOS_RS0)
        cfa = rs0 + (uint32_t)st.cfa_off;
    else
        return u; /* CFA base is a register we don't track: fail the unwind */
    if (!ok)
        return u;

    uint16_t cpc, cs, crs;
    if (!apply_rule(&st.ra, cfa, s16, rs0, readmem, pc, &cpc))
        return u; /* no return address: top of stack */
    if (!apply_rule(&st.s, cfa, s16, rs0, readmem, s16, &cs))
        cs = s16; /* S unchanged if unspecified */
    if (!apply_rule(&st.rs0, cfa, s16, rs0, readmem, rs0, &crs))
        crs = rs0; /* soft SP unchanged if unspecified */

    u.pc = cpc;
    u.s16 = cs;
    u.rs0 = crs;
    u.cfa = (uint16_t)cfa;
    u.ok = true;
    return u;
}

bool dwarf_frame_has(const dwarf_frame_t *df, uint16_t pc)
{
    return df && find_fde(df, pc) != NULL;
}

/* ---- .debug_frame parse ---- */

dwarf_frame_t *dwarf_frame_load(const char *elf_path)
{
    elf_image im;
    if (!elf_open(elf_path, &im))
        return NULL;

    uint32_t fr_off = 0, fr_size = 0;
    if (!elf_find_section(&im, ".debug_frame", &fr_off, &fr_size) ||
        !fr_size || (uint64_t)fr_off + fr_size > (uint64_t)im.size)
    {
        elf_close(&im);
        return NULL;
    }

    dwarf_frame_t *df = calloc(1, sizeof *df);
    if (!df) { elf_close(&im); return NULL; }
    df->frame = malloc(fr_size);
    if (!df->frame) { free(df); elf_close(&im); return NULL; }
    memcpy(df->frame, im.buf + fr_off, fr_size);
    df->addr_size = 4; /* MOS default; overridden per-CIE (v4+) below */
    elf_close(&im);

    const uint8_t *base = df->frame;
    dwarf_cur c = {base, base + fr_size, true};
    while (c.p + 4 <= c.end && c.ok)
    {
        const uint8_t *ent = c.p;
        uint32_t entry_off = (uint32_t)(ent - base);
        uint32_t length = dwarf_u32(&c);
        if (length == 0 || length == 0xffffffffu)
            break; /* terminator / 64-bit DWARF: stop */
        const uint8_t *ent_end = c.p + length;
        if (ent_end > c.end)
            ent_end = c.end;
        uint32_t id = dwarf_u32(&c);
        if (id == 0xffffffffu)
        {
            /* CIE */
            uint8_t version = dwarf_u8(&c);
            const char *aug = (const char *)c.p;
            while (c.p < ent_end && *c.p) c.p++;
            if (c.p < ent_end) c.p++;
            if (version >= 4)
            {
                df->addr_size = dwarf_u8(&c); /* address_size */
                (void)dwarf_u8(&c);           /* segment_selector_size */
            }
            uint32_t code_align = (uint32_t)dwarf_uleb(&c);
            int64_t data_align = dwarf_sleb(&c);
            uint32_t ra = (version >= 3) ? (uint32_t)dwarf_uleb(&c) : dwarf_u8(&c);
            /* We only support the empty augmentation llvm-mos emits. */
            if (aug[0] != 0)
            {
                c.p = ent_end;
                continue;
            }
            cie_t *nc = realloc(df->cies, (df->ncies + 1) * sizeof(cie_t));
            if (!nc) break;
            df->cies = nc;
            cie_t *ci = &df->cies[df->ncies++];
            ci->off = entry_off;
            ci->code_align = code_align ? code_align : 1;
            ci->data_align = data_align;
            ci->ra_column = ra;
            ci->insns = c.p;
            ci->insns_len = (uint32_t)(ent_end - c.p);
        }
        else
        {
            /* FDE: id is the CIE_pointer (offset within .debug_frame). */
            uint32_t initial_loc = (df->addr_size == 8) ? (uint32_t)dwarf_u64(&c) : dwarf_u32(&c);
            uint32_t range = (df->addr_size == 8) ? (uint32_t)dwarf_u64(&c) : dwarf_u32(&c);
            fde_t *nf = realloc(df->fdes, (df->nfdes + 1) * sizeof(fde_t));
            if (!nf) break;
            df->fdes = nf;
            fde_t *fd = &df->fdes[df->nfdes++];
            fd->cie_off = id;
            fd->initial_loc = initial_loc;
            fd->range = range;
            fd->insns = c.p;
            fd->insns_len = (uint32_t)(ent_end - c.p);
        }
        c.p = ent_end;
    }

    if (df->nfdes == 0)
    {
        dwarf_frame_free(df);
        return NULL;
    }
    return df;
}

void dwarf_frame_free(dwarf_frame_t *df)
{
    if (!df)
        return;
    free(df->frame);
    free(df->cies);
    free(df->fdes);
    free(df);
}
