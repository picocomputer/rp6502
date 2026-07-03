/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * DWARF .debug_info reader — see dwarf_info.h. Parses the abbrev table and the
 * DIE tree of each compilation unit (DWARF 2-4, 32-bit) into a small in-memory
 * model: the C type graph, the compilation-unit variables (globals/statics), and
 * each function's locals/parameters with their lexical-scope PC ranges and frame
 * base. The DAP adapter walks this to populate the Variables view.
 *
 * Defensive against truncated/malformed input: a short read aborts the current
 * unit and keeps whatever parsed. The ELF image is read once, parsed, then freed
 * — every string and expression byte the model keeps is copied onto the
 * dwarf_info_t, so queries never touch the file again.
 */

#include "emu/dbg/dwarf_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- DWARF constants (only those we read) ---- */
enum
{
    DW_TAG_array_type = 0x01,
    DW_TAG_enumeration_type = 0x04,
    DW_TAG_formal_parameter = 0x05,
    DW_TAG_lexical_block = 0x0b,
    DW_TAG_member = 0x0d,
    DW_TAG_pointer_type = 0x0f,
    DW_TAG_compile_unit = 0x11,
    DW_TAG_structure_type = 0x13,
    DW_TAG_subroutine_type = 0x15,
    DW_TAG_typedef = 0x16,
    DW_TAG_union_type = 0x17,
    DW_TAG_base_type = 0x24,
    DW_TAG_const_type = 0x26,
    DW_TAG_enumerator = 0x28,
    DW_TAG_subprogram = 0x2e,
    DW_TAG_subrange_type = 0x21,
    DW_TAG_variable = 0x34,
    DW_TAG_volatile_type = 0x35,
    DW_TAG_restrict_type = 0x37,
};
enum
{
    DW_AT_name = 0x03,
    DW_AT_byte_size = 0x0b,
    DW_AT_low_pc = 0x11,
    DW_AT_high_pc = 0x12,
    DW_AT_location = 0x02,
    DW_AT_const_value = 0x1c,
    DW_AT_upper_bound = 0x2f,
    DW_AT_count = 0x37,
    DW_AT_data_member_location = 0x38,
    DW_AT_encoding = 0x3e,
    DW_AT_external = 0x3f,
    DW_AT_frame_base = 0x40,
    DW_AT_type = 0x49,
    DW_AT_declaration = 0x3c,
};
enum
{
    DW_FORM_addr = 0x01,
    DW_FORM_block2 = 0x03,
    DW_FORM_block4 = 0x04,
    DW_FORM_data2 = 0x05,
    DW_FORM_data4 = 0x06,
    DW_FORM_data8 = 0x07,
    DW_FORM_string = 0x08,
    DW_FORM_block = 0x09,
    DW_FORM_block1 = 0x0a,
    DW_FORM_data1 = 0x0b,
    DW_FORM_flag = 0x0c,
    DW_FORM_sdata = 0x0d,
    DW_FORM_strp = 0x0e,
    DW_FORM_udata = 0x0f,
    DW_FORM_ref_addr = 0x10,
    DW_FORM_ref1 = 0x11,
    DW_FORM_ref2 = 0x12,
    DW_FORM_ref4 = 0x13,
    DW_FORM_ref8 = 0x14,
    DW_FORM_ref_udata = 0x15,
    DW_FORM_indirect = 0x16,
    DW_FORM_sec_offset = 0x17,
    DW_FORM_exprloc = 0x18,
    DW_FORM_flag_present = 0x19,
    DW_FORM_ref_sup4 = 0x1c,
    DW_FORM_strp_sup = 0x1d,
    DW_FORM_data16 = 0x1e,
    DW_FORM_line_strp = 0x1f,
};
enum
{
    DW_OP_addr = 0x03,
    DW_OP_plus_uconst = 0x23,
    DW_OP_reg0 = 0x50,
    DW_OP_reg31 = 0x6f,
    DW_OP_regx = 0x90,
    DW_OP_call_frame_cfa = 0x9c,
};

/* ---- persistent model (owned by dwarf_info_t) ---- */
struct dtype
{
    int kind; /* dw_kind_t */
    uint32_t size;
    int encoding;
    char *name; /* interned */
    struct dtype *inner; /* pointee / element / (transparent for qualifiers) */
    uint32_t count;      /* array element count */
    struct dmember
    {
        char *name;
        uint32_t offset;
        struct dtype *type;
    } *members;
    int nmembers;
    struct denum
    {
        char *name;
        int64_t value;
    } *enums;
    int nenums;
};

typedef struct
{
    char *name;
    uint16_t addr;
    struct dtype *type;
    bool addr_ok;
} gvar_t;

typedef struct
{
    char *name;
    struct dtype *type;
    uint8_t *loc;
    uint32_t loc_len;
    uint32_t lo, hi; /* lexical-scope PC range */
} lvar_t;

typedef struct
{
    uint32_t lo, hi;
    uint8_t *fb; /* DW_AT_frame_base expression */
    uint32_t fb_len;
    lvar_t *locals;
    int nlocals;
} func_t;

typedef struct
{
    char *name;
    uint16_t addr;
} sym_t;

struct dwarf_info
{
    char **strs;
    size_t nstrs;
    struct dtype **types; /* every allocated dtype, for free */
    size_t ntypes;
    gvar_t *globals;
    int nglobals;
    func_t *funcs;
    int nfuncs;
    sym_t *syms; /* STT_OBJECT, for the no-location global fallback */
    int nsyms;
};

/* ---- byte cursor ---- */
typedef struct
{
    const uint8_t *p, *end;
    bool ok;
} cur;

static uint8_t u8(cur *c)
{
    if (c->p >= c->end) { c->ok = false; return 0; }
    return *c->p++;
}
static uint16_t u16(cur *c)
{
    uint16_t a = u8(c), b = u8(c);
    return (uint16_t)(a | (b << 8));
}
static uint32_t u32(cur *c)
{
    uint32_t a = u16(c), b = u16(c);
    return a | (b << 16);
}
static uint64_t u64(cur *c)
{
    uint64_t a = u32(c), b = u32(c);
    return a | (b << 32);
}
static uint64_t uleb(cur *c)
{
    uint64_t v = 0;
    int shift = 0;
    for (;;)
    {
        uint8_t b = u8(c);
        if (!c->ok) break;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift > 63) { c->ok = false; break; }
    }
    return v;
}
static int64_t sleb(cur *c)
{
    int64_t v = 0;
    int shift = 0;
    uint8_t b = 0;
    do
    {
        b = u8(c);
        if (!c->ok) break;
        v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40))
        v |= -((int64_t)1 << shift);
    return v;
}

/* uleb/sleb over a raw byte span (for stored expression bytes at query time) */
static uint64_t uleb_raw(const uint8_t *p, const uint8_t *end, const uint8_t **out)
{
    uint64_t v = 0;
    int shift = 0;
    while (p < end)
    {
        uint8_t b = *p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift > 63) break;
    }
    if (out) *out = p;
    return v;
}
static int64_t sleb_raw(const uint8_t *p, const uint8_t *end)
{
    int64_t v = 0;
    int shift = 0;
    uint8_t b = 0;
    while (p < end)
    {
        b = *p++;
        v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    if (shift < 64 && (b & 0x40))
        v |= -((int64_t)1 << shift);
    return v;
}

/* ---- pools ---- */
static char *intern(dwarf_info_t *di, const char *s)
{
    char *dup = strdup(s ? s : "");
    if (!dup) return (char *)"";
    char **ns = realloc(di->strs, (di->nstrs + 1) * sizeof(char *));
    if (!ns) { free(dup); return (char *)""; }
    di->strs = ns;
    di->strs[di->nstrs++] = dup;
    return dup;
}
static struct dtype *new_dtype(dwarf_info_t *di)
{
    struct dtype *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    struct dtype **nt = realloc(di->types, (di->ntypes + 1) * sizeof(struct dtype *));
    if (!nt) { free(t); return NULL; }
    di->types = nt;
    di->types[di->ntypes++] = t;
    return t;
}

/* ---- abbrev table ---- */
typedef struct
{
    uint16_t attr;
    uint16_t form;
} ab_attr;
typedef struct
{
    uint32_t code;
    uint16_t tag;
    bool has_children;
    ab_attr *attrs;
    int nattrs;
} abbrev;
typedef struct
{
    abbrev *list;
    int n;
} abbrev_tab;

static void abbrev_free(abbrev_tab *t)
{
    for (int i = 0; i < t->n; i++)
        free(t->list[i].attrs);
    free(t->list);
    t->list = NULL;
    t->n = 0;
}
static const abbrev *abbrev_find(const abbrev_tab *t, uint32_t code)
{
    for (int i = 0; i < t->n; i++)
        if (t->list[i].code == code)
            return &t->list[i];
    return NULL;
}
static void abbrev_parse(abbrev_tab *t, const uint8_t *base, const uint8_t *end, uint32_t off)
{
    cur c = {base + off, end, true};
    for (;;)
    {
        uint32_t code = (uint32_t)uleb(&c);
        if (!c.ok || code == 0)
            break;
        uint16_t tag = (uint16_t)uleb(&c);
        uint8_t children = u8(&c);
        ab_attr *attrs = NULL;
        int n = 0;
        for (;;)
        {
            uint16_t at = (uint16_t)uleb(&c);
            uint16_t fm = (uint16_t)uleb(&c);
            if (!c.ok || (at == 0 && fm == 0))
                break;
            ab_attr *na = realloc(attrs, (n + 1) * sizeof(ab_attr));
            if (!na) break;
            attrs = na;
            attrs[n].attr = at;
            attrs[n].form = fm;
            n++;
        }
        abbrev *nl = realloc(t->list, (t->n + 1) * sizeof(abbrev));
        if (!nl) { free(attrs); break; }
        t->list = nl;
        t->list[t->n].code = code;
        t->list[t->n].tag = tag;
        t->list[t->n].has_children = children != 0;
        t->list[t->n].attrs = attrs;
        t->list[t->n].nattrs = n;
        t->n++;
    }
}

/* ---- a parsed DIE (transient, per CU) ---- */
typedef struct
{
    uint32_t off; /* .debug_info offset of this DIE */
    uint16_t tag;
    int parent;
    char *name;
    uint32_t type_ref; /* absolute .debug_info offset, 0 if none */
    bool has_type;
    const uint8_t *loc;
    uint32_t loc_len;
    const uint8_t *fb;
    uint32_t fb_len;
    uint32_t low_pc;
    bool has_low;
    uint32_t high_pc;
    bool has_high;
    bool high_is_addr;
    uint32_t byte_size;
    bool has_byte_size;
    int encoding;
    uint64_t count;
    bool has_count;
    uint32_t member_off;
    int64_t const_value;
    bool has_const_value;
    bool external;
} die_t;

typedef struct
{
    die_t *dies;
    int ndies;
    struct dtype **memo; /* per-die memoized type, NULL until built */
    dwarf_info_t *di;
    uint8_t addr_size;
} cu_ctx;

/* a decoded attribute value */
typedef struct
{
    int kind; /* 0 uint, 1 int, 2 str, 3 block, 4 ref(abs off) */
    uint64_t u;
    int64_t s;
    const char *str;
    const uint8_t *block;
    uint32_t blen;
} formval;
enum { FV_U, FV_I, FV_STR, FV_BLOCK, FV_REF };

/* read one attribute value of `form`; cu_off is the CU header start (for refs) */
static void read_form(cur *c, uint16_t form, uint8_t addr_size, uint32_t cu_off,
                      const char *dstr, uint32_t dstr_size,
                      const char *dlstr, uint32_t dlstr_size, formval *v)
{
    memset(v, 0, sizeof *v);
    switch (form)
    {
    case DW_FORM_addr:
        v->kind = FV_U;
        v->u = (addr_size == 8) ? u64(c) : u32(c);
        break;
    case DW_FORM_data1: v->kind = FV_U; v->u = u8(c); break;
    case DW_FORM_data2: v->kind = FV_U; v->u = u16(c); break;
    case DW_FORM_data4: v->kind = FV_U; v->u = u32(c); break;
    case DW_FORM_data8: v->kind = FV_U; v->u = u64(c); break;
    case DW_FORM_sdata: v->kind = FV_I; v->s = sleb(c); break;
    case DW_FORM_udata: v->kind = FV_U; v->u = uleb(c); break;
    case DW_FORM_flag: v->kind = FV_U; v->u = u8(c); break;
    case DW_FORM_flag_present: v->kind = FV_U; v->u = 1; break;
    case DW_FORM_sec_offset: v->kind = FV_U; v->u = u32(c); break;
    case DW_FORM_strp:
    {
        uint32_t o = u32(c);
        v->kind = FV_STR;
        v->str = (o < dstr_size) ? dstr + o : "";
        break;
    }
    case DW_FORM_line_strp:
    {
        uint32_t o = u32(c);
        v->kind = FV_STR;
        v->str = (dlstr && o < dlstr_size) ? dlstr + o : "";
        break;
    }
    case DW_FORM_string:
    {
        v->kind = FV_STR;
        v->str = (const char *)c->p;
        while (c->p < c->end && *c->p) c->p++;
        if (c->p < c->end) c->p++;
        break;
    }
    case DW_FORM_ref1: v->kind = FV_REF; v->u = cu_off + u8(c); break;
    case DW_FORM_ref2: v->kind = FV_REF; v->u = cu_off + u16(c); break;
    case DW_FORM_ref4: v->kind = FV_REF; v->u = cu_off + u32(c); break;
    case DW_FORM_ref8: v->kind = FV_REF; v->u = cu_off + u64(c); break;
    case DW_FORM_ref_udata: v->kind = FV_REF; v->u = cu_off + uleb(c); break;
    case DW_FORM_ref_addr: v->kind = FV_REF; v->u = u32(c); break;
    case DW_FORM_exprloc:
    {
        uint64_t n = uleb(c);
        if (n > (uint64_t)(c->end - c->p)) { c->ok = false; break; }
        v->kind = FV_BLOCK;
        v->block = c->p;
        v->blen = (uint32_t)n;
        c->p += n;
        break;
    }
    case DW_FORM_block1:
    case DW_FORM_block2:
    case DW_FORM_block4:
    case DW_FORM_block:
    {
        uint64_t n = (form == DW_FORM_block1) ? u8(c)
                     : (form == DW_FORM_block2) ? u16(c)
                     : (form == DW_FORM_block4) ? u32(c)
                                                : uleb(c);
        if (n > (uint64_t)(c->end - c->p)) { c->ok = false; break; }
        v->kind = FV_BLOCK;
        v->block = c->p;
        v->blen = (uint32_t)n;
        c->p += n;
        break;
    }
    case DW_FORM_data16:
        if ((uint64_t)16 > (uint64_t)(c->end - c->p)) { c->ok = false; break; }
        v->kind = FV_BLOCK;
        v->block = c->p;
        v->blen = 16;
        c->p += 16;
        break;
    default:
        /* unknown / DWARF5-only form: cannot size it safely */
        c->ok = false;
        break;
    }
}

static int die_index_by_off(const cu_ctx *cu, uint32_t off)
{
    /* dies are appended in increasing .debug_info offset order */
    int lo = 0, hi = cu->ndies;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (cu->dies[mid].off < off) lo = mid + 1;
        else hi = mid;
    }
    if (lo < cu->ndies && cu->dies[lo].off == off)
        return lo;
    return -1;
}

/* ---- type model ---- */
static struct dtype *void_type(dwarf_info_t *di)
{
    struct dtype *t = new_dtype(di);
    if (t) { t->kind = DW_KIND_VOID; t->name = (char *)"void"; }
    return t;
}

static struct dtype *build_type(cu_ctx *cu, int idx);

static uint32_t array_count(cu_ctx *cu, int arr_idx)
{
    /* product of all subrange children's element counts (>=1) */
    uint32_t total = 0;
    for (int i = 0; i < cu->ndies; i++)
    {
        if (cu->dies[i].parent != arr_idx) continue;
        if (cu->dies[i].tag != DW_TAG_subrange_type) continue;
        uint32_t n = cu->dies[i].has_count ? (uint32_t)cu->dies[i].count : 0;
        total = total ? total * (n ? n : 1) : (n ? n : 1);
    }
    return total; /* 0 = unknown length (flexible array) */
}

static struct dtype *build_type(cu_ctx *cu, int idx)
{
    if (idx < 0 || idx >= cu->ndies)
        return NULL;
    if (cu->memo[idx])
        return cu->memo[idx];
    dwarf_info_t *di = cu->di;
    die_t *d = &cu->dies[idx];

    /* qualifiers + typedef are transparent: map straight to the underlying type
     * (its kind/members drive value formatting; the typedef name is dropped). */
    if (d->tag == DW_TAG_typedef || d->tag == DW_TAG_const_type ||
        d->tag == DW_TAG_volatile_type || d->tag == DW_TAG_restrict_type)
    {
        cu->memo[idx] = void_type(di); /* placeholder before recursing (cycle guard) */
        struct dtype *under = d->has_type ? build_type(cu, die_index_by_off(cu, d->type_ref))
                                          : void_type(di);
        cu->memo[idx] = under;
        return under;
    }

    struct dtype *t = new_dtype(di);
    if (!t) return NULL;
    cu->memo[idx] = t; /* memo before recursing (cycle guard) */

    switch (d->tag)
    {
    case DW_TAG_base_type:
        t->kind = DW_KIND_BASE;
        t->size = d->has_byte_size ? d->byte_size : 0;
        t->encoding = d->encoding;
        t->name = d->name ? d->name : (char *)"?";
        break;
    case DW_TAG_pointer_type:
    {
        t->kind = DW_KIND_POINTER;
        t->size = d->has_byte_size ? d->byte_size : 2;
        t->inner = d->has_type ? build_type(cu, die_index_by_off(cu, d->type_ref)) : NULL;
        const char *pt = t->inner && t->inner->name ? t->inner->name : "void";
        char buf[160];
        snprintf(buf, sizeof buf, "%s *", pt);
        t->name = intern(di, buf);
        break;
    }
    case DW_TAG_array_type:
    {
        t->kind = DW_KIND_ARRAY;
        t->inner = d->has_type ? build_type(cu, die_index_by_off(cu, d->type_ref)) : NULL;
        t->count = array_count(cu, idx);
        t->size = (t->inner ? t->inner->size : 0) * t->count;
        const char *et = t->inner && t->inner->name ? t->inner->name : "?";
        char buf[160];
        if (t->count)
            snprintf(buf, sizeof buf, "%s [%u]", et, t->count);
        else
            snprintf(buf, sizeof buf, "%s []", et);
        t->name = intern(di, buf);
        break;
    }
    case DW_TAG_structure_type:
    case DW_TAG_union_type:
    {
        bool uni = d->tag == DW_TAG_union_type;
        t->kind = uni ? DW_KIND_UNION : DW_KIND_STRUCT;
        t->size = d->has_byte_size ? d->byte_size : 0;
        char buf[160];
        snprintf(buf, sizeof buf, "%s %s", uni ? "union" : "struct", d->name ? d->name : "");
        t->name = intern(di, buf);
        for (int i = 0; i < cu->ndies; i++)
        {
            if (cu->dies[i].parent != idx || cu->dies[i].tag != DW_TAG_member)
                continue;
            struct dmember *nm = realloc(t->members, (t->nmembers + 1) * sizeof *nm);
            if (!nm) break;
            t->members = nm;
            t->members[t->nmembers].name = cu->dies[i].name ? cu->dies[i].name : (char *)"";
            t->members[t->nmembers].offset = cu->dies[i].member_off;
            t->members[t->nmembers].type =
                cu->dies[i].has_type ? build_type(cu, die_index_by_off(cu, cu->dies[i].type_ref)) : NULL;
            t->nmembers++;
        }
        break;
    }
    case DW_TAG_enumeration_type:
    {
        t->kind = DW_KIND_ENUM;
        t->size = d->has_byte_size ? d->byte_size : 2;
        char buf[160];
        snprintf(buf, sizeof buf, "enum %s", d->name ? d->name : "");
        t->name = intern(di, buf);
        for (int i = 0; i < cu->ndies; i++)
        {
            if (cu->dies[i].parent != idx || cu->dies[i].tag != DW_TAG_enumerator)
                continue;
            struct denum *ne = realloc(t->enums, (t->nenums + 1) * sizeof *ne);
            if (!ne) break;
            t->enums = ne;
            t->enums[t->nenums].name = cu->dies[i].name ? cu->dies[i].name : (char *)"";
            t->enums[t->nenums].value = cu->dies[i].const_value;
            t->nenums++;
        }
        break;
    }
    case DW_TAG_subroutine_type:
        t->kind = DW_KIND_FUNC;
        t->size = 2;
        t->name = (char *)"()";
        break;
    default:
        t->kind = DW_KIND_UNKNOWN;
        t->name = d->name ? d->name : (char *)"?";
        break;
    }
    return t;
}

/* ---- DIE tree parse for one CU ---- */
static void parse_cu(dwarf_info_t *di, const uint8_t *info, uint32_t cu_off,
                     const uint8_t *cu_data, const uint8_t *cu_end, uint8_t addr_size,
                     const abbrev_tab *ab, const char *dstr, uint32_t dstr_size,
                     const char *dlstr, uint32_t dlstr_size)
{
    cu_ctx cu;
    memset(&cu, 0, sizeof cu);
    cu.di = di;
    cu.addr_size = addr_size;

    cur c = {cu_data, cu_end, true};
    int stack[64];
    const int stack_max = (int)(sizeof stack / sizeof stack[0]);
    int depth = 0;
    int current_parent = -1;

    while (c.p < cu_end && c.ok)
    {
        uint32_t off = (uint32_t)(c.p - info);
        uint32_t code = (uint32_t)uleb(&c);
        if (code == 0)
        {
            if (depth == 0) break;
            /* Track logical depth exactly even past stack_max so pushes and pops
             * stay balanced (a clamped push with an unclamped pop would desync the
             * whole CU); only in-range slots restore an exact parent. */
            depth--;
            if (depth < stack_max)
                current_parent = stack[depth];
            continue;
        }
        const abbrev *a = abbrev_find(ab, code);
        if (!a)
        {
            c.ok = false;
            break;
        }

        die_t d;
        memset(&d, 0, sizeof d);
        d.off = off;
        d.tag = a->tag;
        d.parent = current_parent;

        for (int i = 0; i < a->nattrs && c.ok; i++)
        {
            uint16_t at = a->attrs[i].attr;
            uint16_t fm = a->attrs[i].form;
            if (fm == DW_FORM_indirect)
                fm = (uint16_t)uleb(&c);
            formval v;
            read_form(&c, fm, addr_size, cu_off, dstr, dstr_size, dlstr, dlstr_size, &v);
            if (!c.ok) break;
            switch (at)
            {
            case DW_AT_name:
                if (v.kind == FV_STR) d.name = intern(di, v.str);
                break;
            case DW_AT_type:
                d.type_ref = (uint32_t)v.u;
                d.has_type = true;
                break;
            case DW_AT_location:
                if (v.kind == FV_BLOCK) { d.loc = v.block; d.loc_len = v.blen; }
                break;
            case DW_AT_frame_base:
                if (v.kind == FV_BLOCK) { d.fb = v.block; d.fb_len = v.blen; }
                break;
            case DW_AT_low_pc:
                d.low_pc = (uint32_t)v.u;
                d.has_low = true;
                break;
            case DW_AT_high_pc:
                d.high_pc = (uint32_t)v.u;
                d.has_high = true;
                d.high_is_addr = (fm == DW_FORM_addr);
                break;
            case DW_AT_byte_size:
                d.byte_size = (uint32_t)v.u;
                d.has_byte_size = true;
                break;
            case DW_AT_encoding:
                d.encoding = (int)v.u;
                break;
            case DW_AT_count:
                d.count = v.u;
                d.has_count = true;
                break;
            case DW_AT_upper_bound:
                if (!d.has_count) { d.count = v.u + 1; d.has_count = true; }
                break;
            case DW_AT_data_member_location:
                if (v.kind == FV_BLOCK)
                {
                    /* DW_OP_plus_uconst <off> */
                    if (v.blen >= 1 && v.block[0] == DW_OP_plus_uconst)
                        d.member_off = (uint32_t)uleb_raw(v.block + 1, v.block + v.blen, NULL);
                }
                else
                    d.member_off = (v.kind == FV_I) ? (uint32_t)v.s : (uint32_t)v.u;
                break;
            case DW_AT_const_value:
                d.const_value = (v.kind == FV_I) ? v.s : (int64_t)v.u;
                d.has_const_value = true;
                break;
            case DW_AT_external:
                d.external = v.u != 0;
                break;
            default:
                break;
            }
        }
        if (!c.ok)
            break;

        die_t *nd = realloc(cu.dies, (cu.ndies + 1) * sizeof(die_t));
        if (!nd) break;
        cu.dies = nd;
        cu.dies[cu.ndies] = d;
        int idx = cu.ndies;
        cu.ndies++;

        if (a->has_children)
        {
            if (depth < stack_max)
                stack[depth] = current_parent;
            depth++; /* advance even when the slot is out of range (see pop) */
            current_parent = idx;
        }
    }

    if (cu.ndies == 0)
        return;

    cu.memo = calloc(cu.ndies, sizeof(struct dtype *));
    if (!cu.memo) { free(cu.dies); return; }

    /* compilation-unit root (for global scope) */
    int cu_root = -1;
    for (int i = 0; i < cu.ndies; i++)
        if (cu.dies[i].tag == DW_TAG_compile_unit) { cu_root = i; break; }

    /* globals: named variables directly under the CU root */
    for (int i = 0; i < cu.ndies; i++)
    {
        die_t *d = &cu.dies[i];
        if (d->tag != DW_TAG_variable || d->parent != cu_root || !d->name)
            continue;
        gvar_t g;
        memset(&g, 0, sizeof g);
        g.name = d->name;
        g.type = d->has_type ? build_type(&cu, die_index_by_off(&cu, d->type_ref)) : NULL;
        if (d->loc && d->loc_len >= 1 && d->loc[0] == DW_OP_addr)
        {
            uint32_t a = 0;
            for (uint32_t k = 0; k + 1 <= d->loc_len - 1 && k < 4; k++)
                a |= (uint32_t)d->loc[1 + k] << (8 * k);
            g.addr = (uint16_t)a;
            g.addr_ok = true;
        }
        else
        {
            /* no location (e.g. GC'd or declaration): fall back to .symtab */
            for (int s = 0; s < di->nsyms; s++)
                if (strcmp(di->syms[s].name, d->name) == 0)
                {
                    g.addr = di->syms[s].addr;
                    g.addr_ok = true;
                    break;
                }
        }
        gvar_t *ng = realloc(di->globals, (di->nglobals + 1) * sizeof(gvar_t));
        if (!ng) break;
        di->globals = ng;
        di->globals[di->nglobals++] = g;
    }

    /* functions + their locals/parameters */
    for (int i = 0; i < cu.ndies; i++)
    {
        die_t *sp = &cu.dies[i];
        if (sp->tag != DW_TAG_subprogram || !sp->has_low || !sp->has_high)
            continue;
        func_t fn;
        memset(&fn, 0, sizeof fn);
        fn.lo = sp->low_pc;
        fn.hi = sp->high_is_addr ? sp->high_pc : sp->low_pc + sp->high_pc;
        if (sp->fb && sp->fb_len)
        {
            fn.fb = malloc(sp->fb_len);
            if (fn.fb) { memcpy(fn.fb, sp->fb, sp->fb_len); fn.fb_len = sp->fb_len; }
        }
        /* collect descendant variables/parameters */
        for (int j = 0; j < cu.ndies; j++)
        {
            die_t *v = &cu.dies[j];
            if (v->tag != DW_TAG_variable && v->tag != DW_TAG_formal_parameter)
                continue;
            if (!v->name || !v->loc || !v->loc_len)
                continue;
            /* is j a descendant of subprogram i? */
            int p = v->parent;
            bool desc = false;
            while (p >= 0)
            {
                if (p == i) { desc = true; break; }
                p = cu.dies[p].parent;
            }
            if (!desc)
                continue;
            /* lexical scope: nearest ancestor (incl. self's parents) with a PC range */
            uint32_t lo = fn.lo, hi = fn.hi;
            for (int q = v->parent; q >= 0; q = cu.dies[q].parent)
            {
                if (cu.dies[q].has_low && cu.dies[q].has_high)
                {
                    lo = cu.dies[q].low_pc;
                    hi = cu.dies[q].high_is_addr ? cu.dies[q].high_pc
                                                 : cu.dies[q].low_pc + cu.dies[q].high_pc;
                    break;
                }
                if (q == i) break;
            }
            lvar_t lv;
            memset(&lv, 0, sizeof lv);
            lv.name = v->name;
            lv.type = v->has_type ? build_type(&cu, die_index_by_off(&cu, v->type_ref)) : NULL;
            lv.loc = malloc(v->loc_len);
            if (!lv.loc) continue;
            memcpy(lv.loc, v->loc, v->loc_len);
            lv.loc_len = v->loc_len;
            lv.lo = lo;
            lv.hi = hi;
            lvar_t *nl = realloc(fn.locals, (fn.nlocals + 1) * sizeof(lvar_t));
            if (!nl) { free(lv.loc); break; }
            fn.locals = nl;
            fn.locals[fn.nlocals++] = lv;
        }
        func_t *nf = realloc(di->funcs, (di->nfuncs + 1) * sizeof(func_t));
        if (!nf) { free(fn.fb); free(fn.locals); break; }
        di->funcs = nf;
        di->funcs[di->nfuncs++] = fn;
    }

    free(cu.memo);
    free(cu.dies);
}

/* ---- .symtab STT_OBJECT (the no-location global fallback) ---- */
static void parse_objects(dwarf_info_t *di, const uint8_t *buf, long sz,
                          uint32_t sym_off, uint32_t sym_size,
                          uint32_t str_off, uint32_t str_size)
{
    if (!sym_off || !str_off) return;
    if ((uint64_t)sym_off + sym_size > (uint64_t)sz ||
        (uint64_t)str_off + str_size > (uint64_t)sz)
        return;
    const char *strtab = (const char *)(buf + str_off);
    for (uint32_t o = 0; o + 16 <= sym_size; o += 16)
    {
        const uint8_t *s = buf + sym_off + o;
        uint32_t st_name = s[0] | (s[1] << 8) | (s[2] << 16) | ((uint32_t)s[3] << 24);
        uint32_t st_value = s[4] | (s[5] << 8) | (s[6] << 16) | ((uint32_t)s[7] << 24);
        uint8_t st_info = s[12];
        if ((st_info & 0xf) != 1 /*STT_OBJECT*/ || st_name >= str_size)
            continue;
        const char *nm = strtab + st_name;
        if (!nm[0])
            continue;
        sym_t *ns = realloc(di->syms, (di->nsyms + 1) * sizeof(sym_t));
        if (!ns) break;
        di->syms = ns;
        di->syms[di->nsyms].name = intern(di, nm);
        di->syms[di->nsyms].addr = (uint16_t)st_value;
        di->nsyms++;
    }
}

#define SH_U32(idx, off)                                                  \
    ((uint32_t)(buf[e_shoff + (idx) * e_shentsize + (off)] |              \
                (buf[e_shoff + (idx) * e_shentsize + (off) + 1] << 8) |   \
                (buf[e_shoff + (idx) * e_shentsize + (off) + 2] << 16) |  \
                ((uint32_t)buf[e_shoff + (idx) * e_shentsize + (off) + 3] << 24)))

dwarf_info_t *dwarf_info_load(const char *elf_path)
{
    FILE *f = fopen(elf_path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 64) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[sz] = 0; /* terminate any unterminated string at end of file */
    fclose(f);

    if (memcmp(buf, "\x7f""ELF", 4) != 0 || buf[4] != 1 || buf[5] != 1)
    {
        free(buf);
        return NULL;
    }
    uint32_t e_shoff = buf[32] | (buf[33] << 8) | (buf[34] << 16) | ((uint32_t)buf[35] << 24);
    uint16_t e_shentsize = buf[46] | (buf[47] << 8);
    uint16_t e_shnum = buf[48] | (buf[49] << 8);
    uint16_t e_shstrndx = buf[50] | (buf[51] << 8);
    if (e_shoff == 0 || e_shentsize < 40 || e_shstrndx >= e_shnum)
    {
        free(buf);
        return NULL;
    }
    if ((uint64_t)e_shoff + (uint64_t)e_shnum * (uint64_t)e_shentsize > (uint64_t)sz)
    {
        free(buf);
        return NULL;
    }
    uint32_t shstr_off = SH_U32(e_shstrndx, 16);
    if (shstr_off >= (uint64_t)sz)
    {
        free(buf);
        return NULL;
    }
    const char *shstr = (const char *)(buf + shstr_off);

/* Section name at shstr+SH_U32(i,0), clamped so strcmp never walks past buf. */
#define SH_NAME(idx)                                            \
    ((SH_U32(idx, 0) < (uint64_t)sz - shstr_off)                \
         ? shstr + SH_U32(idx, 0)                               \
         : "")

    uint32_t info_off = 0, info_size = 0, abbrev_off = 0, abbrev_size = 0;
    uint32_t str_off = 0, str_size = 0, lstr_off = 0, lstr_size = 0;
    uint32_t sym_off = 0, sym_size = 0, symstr_off = 0, symstr_size = 0;
    for (uint16_t i = 0; i < e_shnum; i++)
    {
        const char *nm = SH_NAME(i);
        uint32_t off = SH_U32(i, 16), size = SH_U32(i, 20);
        if (strcmp(nm, ".debug_info") == 0) { info_off = off; info_size = size; }
        else if (strcmp(nm, ".debug_abbrev") == 0) { abbrev_off = off; abbrev_size = size; }
        else if (strcmp(nm, ".debug_str") == 0) { str_off = off; str_size = size; }
        else if (strcmp(nm, ".debug_line_str") == 0) { lstr_off = off; lstr_size = size; }
        else if (strcmp(nm, ".symtab") == 0) { sym_off = off; sym_size = size; }
        else if (strcmp(nm, ".strtab") == 0) { symstr_off = off; symstr_size = size; }
    }
    if (!info_off || !info_size || !abbrev_off ||
        (uint64_t)info_off + info_size > (uint64_t)sz ||
        (uint64_t)abbrev_off + abbrev_size > (uint64_t)sz)
    {
        free(buf);
        return NULL;
    }
    /* A .debug_str/.debug_line_str whose [off, off+size) runs past EOF would let a
     * DW_FORM_strp/line_strp offset (read_form bounds it only against the section
     * size) dereference outside the file buffer. Neutralize an out-of-range table
     * so those forms resolve to "" instead of reading unmapped memory. */
    if (str_off && (uint64_t)str_off + str_size > (uint64_t)sz)
        str_off = str_size = 0;
    if (lstr_off && (uint64_t)lstr_off + lstr_size > (uint64_t)sz)
        lstr_off = lstr_size = 0;

    dwarf_info_t *di = calloc(1, sizeof *di);
    if (!di) { free(buf); return NULL; }

    parse_objects(di, buf, sz, sym_off, sym_size, symstr_off, symstr_size);

    const char *dstr = str_off ? (const char *)(buf + str_off) : "";
    const char *dlstr = lstr_off ? (const char *)(buf + lstr_off) : NULL;
    const uint8_t *info = buf + info_off;
    const uint8_t *info_end = info + info_size;
    const uint8_t *ab_base = buf + abbrev_off;
    const uint8_t *ab_end = ab_base + abbrev_size;

    /* walk the compilation units */
    cur c = {info, info_end, true};
    while (c.p + 4 <= info_end && c.ok)
    {
        const uint8_t *unit_start = c.p;
        uint32_t cu_off = (uint32_t)(unit_start - info);
        uint32_t unit_len = u32(&c);
        if (unit_len == 0 || unit_len == 0xffffffffu)
            break;
        const uint8_t *unit_end = unit_start + 4 + unit_len;
        if (unit_end > info_end) unit_end = info_end;

        uint16_t version = u16(&c);
        uint32_t ab_off;
        uint8_t addr_size;
        if (version >= 2 && version <= 4)
        {
            ab_off = u32(&c);
            addr_size = u8(&c);
        }
        else
        {
            /* DWARF5+ header differs; skip this unit */
            c.p = unit_end;
            continue;
        }
        if (!c.ok) break;

        abbrev_tab ab;
        memset(&ab, 0, sizeof ab);
        abbrev_parse(&ab, ab_base, ab_end, ab_off);
        parse_cu(di, info, cu_off, c.p, unit_end, addr_size, &ab,
                 dstr, str_size, dlstr, lstr_size);
        abbrev_free(&ab);

        c.p = unit_end;
    }

    free(buf);

    if (di->nglobals == 0 && di->nfuncs == 0)
    {
        dwarf_info_free(di);
        return NULL;
    }
    return di;
}

void dwarf_info_free(dwarf_info_t *di)
{
    if (!di) return;
    for (size_t i = 0; i < di->nstrs; i++)
        free(di->strs[i]);
    free(di->strs);
    for (size_t i = 0; i < di->ntypes; i++)
    {
        free(di->types[i]->members);
        free(di->types[i]->enums);
        free(di->types[i]);
    }
    free(di->types);
    free(di->globals);
    for (int i = 0; i < di->nfuncs; i++)
    {
        free(di->funcs[i].fb);
        for (int j = 0; j < di->funcs[i].nlocals; j++)
            free(di->funcs[i].locals[j].loc);
        free(di->funcs[i].locals);
    }
    free(di->funcs);
    free(di->syms);
    free(di);
}

int dwarf_info_globals(const dwarf_info_t *di, dwarf_var_t *out, int max)
{
    if (!di) return 0;
    int n = 0;
    for (int i = 0; i < di->nglobals && n < max; i++)
    {
        out[n].name = di->globals[i].name;
        out[n].addr = di->globals[i].addr;
        out[n].type = di->globals[i].type;
        out[n].addr_ok = di->globals[i].addr_ok;
        n++;
    }
    return n;
}

/* Evaluate a function frame base into a 6502 address (the soft stack pointer). */
static bool frame_base_value(const func_t *fn, uint8_t (*readmem)(uint16_t), uint16_t *out)
{
    if (!fn->fb || fn->fb_len < 1)
        return false;
    uint8_t op = fn->fb[0];
    uint32_t zp;
    if (op == DW_OP_regx)
    {
        const uint8_t *p = fn->fb + 1;
        uint64_t reg = uleb_raw(p, fn->fb + fn->fb_len, NULL);
        /* llvm-mos: RS0 (the rc0:rc1 soft stack pointer pair) is DWARF reg 528;
         * RSn lives at zero page 2n. Anything else falls back to rc0:rc1. */
        zp = (reg >= 528) ? (uint32_t)((reg - 528) * 2) : 0;
    }
    else if (op >= DW_OP_reg0 && op <= DW_OP_reg31)
    {
        zp = 0; /* a bare register frame base -> the soft stack pointer */
    }
    else
    {
        /* DW_OP_call_frame_cfa and friends need .debug_frame, which llvm-mos
         * does not emit for this target. */
        return false;
    }
    if (zp > 0xFE)
        return false;
    *out = (uint16_t)(readmem((uint16_t)zp) | (readmem((uint16_t)(zp + 1)) << 8));
    return true;
}

int dwarf_info_locals(const dwarf_info_t *di, uint16_t pc,
                      uint8_t (*readmem)(uint16_t addr),
                      dwarf_var_t *out, int max)
{
    if (!di || !readmem) return 0;
    const func_t *fn = NULL;
    for (int i = 0; i < di->nfuncs; i++)
        if (pc >= di->funcs[i].lo && pc < di->funcs[i].hi) { fn = &di->funcs[i]; break; }
    if (!fn) return 0;

    uint16_t fb = 0;
    bool fb_ok = frame_base_value(fn, readmem, &fb);

    int n = 0;
    for (int i = 0; i < fn->nlocals && n < max; i++)
    {
        const lvar_t *v = &fn->locals[i];
        if (pc < v->lo || pc >= v->hi)
            continue;
        dwarf_var_t r;
        r.name = v->name;
        r.type = v->type;
        r.addr = 0;
        r.addr_ok = false;
        if (v->loc_len >= 1)
        {
            uint8_t op = v->loc[0];
            if (op == DW_OP_addr)
            {
                uint32_t a = 0;
                for (uint32_t k = 0; k + 1 <= v->loc_len - 1 && k < 4; k++)
                    a |= (uint32_t)v->loc[1 + k] << (8 * k);
                r.addr = (uint16_t)a;
                r.addr_ok = true;
            }
            else if (op == 0x91 /*DW_OP_fbreg*/ && fb_ok)
            {
                int64_t off = sleb_raw(v->loc + 1, v->loc + v->loc_len);
                r.addr = (uint16_t)((int32_t)fb + (int32_t)off);
                r.addr_ok = true;
            }
        }
        out[n++] = r;
    }
    return n;
}

/* ---- type introspection ---- */
dw_kind_t dwarf_type_kind(const dtype_t *t) { return t ? (dw_kind_t)t->kind : DW_KIND_UNKNOWN; }
uint32_t dwarf_type_size(const dtype_t *t) { return t ? t->size : 0; }
const char *dwarf_type_name(const dtype_t *t) { return t && t->name ? t->name : "?"; }
int dwarf_type_encoding(const dtype_t *t) { return t ? t->encoding : 0; }

const dtype_t *dwarf_type_pointee(const dtype_t *t) { return t ? t->inner : NULL; }

const dtype_t *dwarf_type_element(const dtype_t *t, uint32_t *count)
{
    if (!t) return NULL;
    if (count) *count = t->count;
    return t->inner;
}

int dwarf_type_member_count(const dtype_t *t) { return t ? t->nmembers : 0; }

bool dwarf_type_member(const dtype_t *t, int i, const char **name,
                       uint32_t *offset, const dtype_t **type)
{
    if (!t || i < 0 || i >= t->nmembers)
        return false;
    if (name) *name = t->members[i].name;
    if (offset) *offset = t->members[i].offset;
    if (type) *type = t->members[i].type;
    return true;
}

const char *dwarf_type_enum_name(const dtype_t *t, int64_t value)
{
    if (!t) return NULL;
    for (int i = 0; i < t->nenums; i++)
        if (t->enums[i].value == value)
            return t->enums[i].name;
    return NULL;
}
