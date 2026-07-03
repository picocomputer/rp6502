/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * cc65 .dbg reader — see cc65dbg.h. The file is a flat list of text records,
 * one per line, "type<TAB>key=val,key=val,...". We need:
 *   seg  id,name,start          - a segment's absolute load address
 *   span id,seg,start,size      - a code span: offset+size within a segment
 *   file id,name                - a source file
 *   line file,line,type,span    - a source line; type=1 is C (vs 0=asm/2=macro),
 *                                 span is a '+'-separated list of span ids
 *   sym  name,val,seg,type=lab  - a label; "_name" in a CODE segment is a func
 * A C line's address = seg[span.seg].start + span.start. We keep only C lines so
 * a PC maps back to the .c the developer wrote, not the temporary .s.
 */

#include "emu/dbg/cc65dbg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    uint32_t addr;
    uint32_t size;
    const char *file;
    int line;
} cc_row;

typedef struct
{
    uint32_t addr;
    const char *name;
} cc_func;

/* a lexical scope's PC range (union of its spans), indexed by cc65 scope id */
typedef struct
{
    uint32_t lo, hi;
    bool has;
} cc_scope;

/* a C symbol (csym): an auto local (addr = c_sp + offs) or a global (fixed) */
typedef struct
{
    const char *name;
    uint32_t scope;
    bool is_auto;
    bool is_global;
    int32_t offs;  /* auto: frame offset relative to c_sp */
    uint32_t addr; /* global: fixed 6502 address */
} cc_csym;

/* a linker segment (CODE/DATA/BSS/...) with its load address + size */
typedef struct
{
    const char *name;
    uint32_t start;
    uint32_t size;
} cc_seg;

struct cc65dbg
{
    cc_row *rows;
    size_t nrows;
    cc_func *funcs;
    size_t nfuncs;
    cc_scope *scopes;
    size_t nscopes;
    cc_csym *csyms;
    size_t ncsyms;
    cc_seg *segs;
    size_t nsegs;
    uint16_t c_sp; /* zero-page address of the C stack pointer */
    bool has_c_sp;
    char **strs;
    size_t nstrs;
};

/* ---- field access over one record's "key=val,key=val,..." body ---- */

/* Locate key's value (quotes stripped). Matches a whole key (start-or-comma to
 * '='), so "line" never matches inside another field. */
static bool field(const char *rec, const char *key, const char **vs, size_t *vl)
{
    size_t klen = strlen(key);
    const char *p = rec;
    while (*p)
    {
        const char *eq = p;
        while (*eq && *eq != '=' && *eq != ',')
            eq++;
        if (*eq == '=')
        {
            size_t fklen = (size_t)(eq - p);
            const char *v = eq + 1, *q = v;
            bool inq = false;
            while (*q)
            {
                if (*q == '"')
                    inq = !inq;
                else if (*q == ',' && !inq)
                    break;
                q++;
            }
            if (fklen == klen && strncmp(p, key, klen) == 0)
            {
                *vs = v;
                *vl = (size_t)(q - v);
                if (*vl >= 2 && v[0] == '"' && v[*vl - 1] == '"')
                {
                    (*vs)++;
                    *vl -= 2;
                }
                return true;
            }
            p = (*q == ',') ? q + 1 : q;
        }
        else
            p = (*eq == ',') ? eq + 1 : eq;
    }
    return false;
}

static uint32_t to_u32(const char *s, size_t n)
{
    uint32_t v = 0;
    size_t i = 0;
    bool hex = (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
    if (hex)
        i = 2;
    for (; i < n; i++)
    {
        char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9')
            d = (uint32_t)(c - '0');
        else if (hex && c >= 'a' && c <= 'f')
            d = (uint32_t)(c - 'a' + 10);
        else if (hex && c >= 'A' && c <= 'F')
            d = (uint32_t)(c - 'A' + 10);
        else
            break;
        v = v * (hex ? 16u : 10u) + d;
    }
    return v;
}

/* key -> u32; def if absent. */
static uint32_t fu32(const char *rec, const char *key, uint32_t def)
{
    const char *v;
    size_t n;
    return field(rec, key, &v, &n) ? to_u32(v, n) : def;
}

/* key -> signed int; def if absent (csym "offs" may be negative). */
static int32_t fi32(const char *rec, const char *key, int32_t def)
{
    const char *v;
    size_t n;
    if (!field(rec, key, &v, &n) || n == 0)
        return def;
    bool neg = v[0] == '-';
    if (neg || v[0] == '+')
    {
        v++;
        n--;
    }
    int32_t m = (int32_t)to_u32(v, n);
    return neg ? -m : m;
}

static const char *intern(cc65dbg_t *db, const char *s, size_t n)
{
    char *dup = malloc(n + 1);
    if (!dup)
        return "";
    memcpy(dup, s, n);
    dup[n] = 0;
    char **ns = realloc(db->strs, (db->nstrs + 1) * sizeof(char *));
    if (!ns)
    {
        free(dup);
        return "";
    }
    db->strs = ns;
    db->strs[db->nstrs++] = dup;
    return dup;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    const char *b = strrchr(p, '\\');
    if (b && (!s || b > s))
        s = b;
    return s ? s + 1 : p;
}

static bool rec_is(const char *line, const char *type, const char **body)
{
    size_t n = strlen(type);
    if (strncmp(line, type, n) == 0 && line[n] == '\t')
    {
        *body = line + n + 1;
        return true;
    }
    return false;
}

/* sym-table flag bits (transient, during load) */
enum { SYM_LAB = 1, SYM_CODE = 2, SYM_IMP = 4 };

/* Resolve a sym id to its defining label, following one or more import->export
 * hops. Returns false if it doesn't resolve to a label. */
static bool sym_resolve(const uint32_t *symval, const uint32_t *symexp,
                        const uint8_t *symflags, uint32_t nsym, uint32_t id,
                        uint32_t *addr, bool *is_code)
{
    for (int hop = 0; hop < 8 && id < nsym; hop++)
    {
        if (symflags[id] & SYM_LAB)
        {
            *addr = symval[id];
            *is_code = (symflags[id] & SYM_CODE) != 0;
            return true;
        }
        if (symflags[id] & SYM_IMP)
            id = symexp[id];
        else
            break;
    }
    return false;
}

static int row_cmp(const void *a, const void *b)
{
    uint32_t x = ((const cc_row *)a)->addr, y = ((const cc_row *)b)->addr;
    return (x > y) - (x < y);
}
static int func_cmp(const void *a, const void *b)
{
    uint32_t x = ((const cc_func *)a)->addr, y = ((const cc_func *)b)->addr;
    return (x > y) - (x < y);
}

cc65dbg_t *cc65dbg_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0)
    {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[sz] = 0;

    /* Split into NUL-terminated lines. */
    char **lines = NULL;
    size_t nlines = 0, cap = 0;
    for (char *p = buf; p < buf + sz;)
    {
        char *nl = memchr(p, '\n', (size_t)(buf + sz - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(buf + sz - p);
        if (len && p[len - 1] == '\r')
            p[len - 1] = 0;
        if (nl)
            *nl = 0;
        if (nlines == cap)
        {
            cap = cap ? cap * 2 : 256;
            char **nlp = realloc(lines, cap * sizeof(char *));
            if (!nlp)
                break;
            lines = nlp;
        }
        lines[nlines++] = p;
        p = nl ? nl + 1 : buf + sz;
    }

    /* Sizing from the "info" record. */
    uint32_t nfile = 0, nseg = 0, nspan = 0, nsym = 0, nscope = 0;
    for (size_t i = 0; i < nlines; i++)
    {
        const char *body;
        if (rec_is(lines[i], "info", &body))
        {
            nfile = fu32(body, "file", 0);
            nseg = fu32(body, "seg", 0);
            nspan = fu32(body, "span", 0);
            nsym = fu32(body, "sym", 0);
            nscope = fu32(body, "scope", 0);
            break;
        }
    }

    cc65dbg_t *db = calloc(1, sizeof *db);
    const char **files = nfile ? calloc(nfile, sizeof(char *)) : NULL;
    uint32_t *segstart = nseg ? calloc(nseg, sizeof(uint32_t)) : NULL;
    uint8_t *segcode = nseg ? calloc(nseg, sizeof(uint8_t)) : NULL;
    struct span_t
    {
        uint32_t seg, start, size;
    } *spans = nspan ? calloc(nspan, sizeof(struct span_t)) : NULL;
    /* transient: a sym id -> {address, flags, export-target} table, to resolve
     * csym globals (data labels) and tell function csyms (CODE labels) apart. A
     * csym usually points at an import sym, which chains via "exp" to the lab. */
    uint32_t *symval = nsym ? calloc(nsym, sizeof(uint32_t)) : NULL;
    uint32_t *symexp = nsym ? calloc(nsym, sizeof(uint32_t)) : NULL;
    uint8_t *symflags = nsym ? calloc(nsym, sizeof(uint8_t)) : NULL; /* see SYM_* above */
    if (db && nscope)
        db->scopes = calloc(nscope, sizeof(cc_scope));
    if (db && nseg)
        db->segs = calloc(nseg, sizeof(cc_seg));
    if (!db || (nfile && !files) || (nseg && (!segstart || !segcode || !db->segs)) || (nspan && !spans) ||
        (nsym && (!symval || !symexp || !symflags)) || (nscope && !db->scopes))
    {
        free(files);
        free(segstart);
        free(segcode);
        free(spans);
        free(symval);
        free(symexp);
        free(symflags);
        free(lines);
        free(buf);
        if (db)
        {
            free(db->scopes);
            free(db->segs);
        }
        free(db);
        return NULL;
    }
    db->nscopes = nscope;
    db->nsegs = nseg;

    /* Pass 1: file / seg / span / sym (the tables line records reference). */
    for (size_t i = 0; i < nlines; i++)
    {
        const char *body;
        char *line = lines[i];
        if (rec_is(line, "file", &body))
        {
            uint32_t id = fu32(body, "id", 0xffffffff);
            const char *v;
            size_t n;
            if (id < nfile && field(body, "name", &v, &n))
                files[id] = intern(db, v, n);
        }
        else if (rec_is(line, "seg", &body))
        {
            uint32_t id = fu32(body, "id", 0xffffffff);
            if (id < nseg)
            {
                segstart[id] = fu32(body, "start", 0);
                const char *v;
                size_t n;
                bool has_name = field(body, "name", &v, &n);
                segcode[id] = has_name && n == 4 && strncmp(v, "CODE", 4) == 0;
                db->segs[id].name = has_name ? intern(db, v, n) : NULL;
                db->segs[id].start = segstart[id];
                db->segs[id].size = fu32(body, "size", 0);
            }
        }
        else if (rec_is(line, "span", &body))
        {
            uint32_t id = fu32(body, "id", 0xffffffff);
            if (id < nspan)
            {
                spans[id].seg = fu32(body, "seg", 0xffffffff);
                spans[id].start = fu32(body, "start", 0);
                spans[id].size = fu32(body, "size", 0);
            }
        }
        else if (rec_is(line, "sym", &body))
        {
            const char *tv, *nv;
            size_t tn, nn;
            bool is_lab = field(body, "type", &tv, &tn) && tn == 3 && strncmp(tv, "lab", 3) == 0;
            bool is_imp = field(body, "type", &tv, &tn) && tn == 3 && strncmp(tv, "imp", 3) == 0;
            bool has_name = field(body, "name", &nv, &nn);
            uint32_t id = fu32(body, "id", 0xffffffff);
            uint32_t seg = fu32(body, "seg", 0xffffffff);
            uint32_t val = fu32(body, "val", 0);
            bool is_code = seg < nseg && segcode[seg];
            /* record into the sym table (for csym resolution) */
            if (id < nsym)
            {
                if (is_lab)
                {
                    symval[id] = val;
                    symflags[id] = (uint8_t)(SYM_LAB | (is_code ? SYM_CODE : 0));
                }
                else if (is_imp)
                {
                    symexp[id] = fu32(body, "exp", 0xffffffff);
                    symflags[id] = SYM_IMP;
                }
            }
            if (!is_lab)
                continue;
            /* the C stack pointer's zero-page address (auto-local frame base) */
            if (has_name && nn == 4 && strncmp(nv, "c_sp", 4) == 0)
            {
                db->c_sp = (uint16_t)val;
                db->has_c_sp = true;
            }
            /* a "_name" CODE label is a function */
            if (has_name && nn >= 2 && nv[0] == '_' && is_code)
            {
                cc_func *nf = realloc(db->funcs, (db->nfuncs + 1) * sizeof(cc_func));
                if (nf)
                {
                    db->funcs = nf;
                    db->funcs[db->nfuncs].addr = val;
                    db->funcs[db->nfuncs].name = intern(db, nv + 1, nn - 1); /* strip '_' */
                    db->nfuncs++;
                }
            }
        }
    }

    /* Pass 1b: scopes (PC ranges) + csyms (C variables). Needs the seg/span and
     * sym tables from pass 1, so it runs after that completes. */
    for (size_t i = 0; i < nlines; i++)
    {
        const char *body;
        char *line = lines[i];
        if (rec_is(line, "scope", &body))
        {
            uint32_t id = fu32(body, "id", 0xffffffff);
            const char *sv;
            size_t sn;
            if (id >= nscope || !field(body, "span", &sv, &sn))
                continue; /* scopes without spans (e.g. struct) carry no PC range */
            const char *p = sv, *end = sv + sn;
            while (p < end)
            {
                uint32_t sid = 0;
                bool any = false;
                while (p < end && *p >= '0' && *p <= '9')
                {
                    sid = sid * 10 + (uint32_t)(*p - '0');
                    p++;
                    any = true;
                }
                if (any && sid < nspan && spans[sid].seg < nseg)
                {
                    uint32_t lo = segstart[spans[sid].seg] + spans[sid].start;
                    uint32_t hi = lo + (spans[sid].size ? spans[sid].size : 1);
                    cc_scope *s = &db->scopes[id];
                    if (!s->has) { s->lo = lo; s->hi = hi; s->has = true; }
                    else { if (lo < s->lo) s->lo = lo; if (hi > s->hi) s->hi = hi; }
                }
                if (p < end && *p == '+') p++;
                else break;
            }
        }
        else if (rec_is(line, "csym", &body))
        {
            const char *nv;
            size_t nn;
            if (!field(body, "name", &nv, &nn))
                continue;
            const char *scv;
            size_t scn;
            bool is_auto = false, is_global = false;
            if (field(body, "sc", &scv, &scn))
            {
                if (scn == 4 && strncmp(scv, "auto", 4) == 0)
                    is_auto = true;
                else if ((scn == 3 && strncmp(scv, "ext", 3) == 0) ||
                         (scn == 6 && strncmp(scv, "static", 6) == 0))
                    is_global = true;
            }
            cc_csym cs;
            memset(&cs, 0, sizeof cs);
            cs.name = intern(db, nv, nn);
            cs.scope = fu32(body, "scope", 0xffffffff);
            if (is_auto)
            {
                cs.is_auto = true;
                cs.offs = fi32(body, "offs", 0);
            }
            else if (is_global)
            {
                /* a global is a data label; a function csym resolves to a CODE
                 * label and is excluded. */
                uint32_t symid = fu32(body, "sym", 0xffffffff);
                uint32_t addr;
                bool is_code;
                if (sym_resolve(symval, symexp, symflags, nsym, symid, &addr, &is_code) &&
                    !is_code)
                {
                    cs.is_global = true;
                    cs.addr = addr;
                }
                else
                    continue;
            }
            else
                continue;
            cc_csym *nc = realloc(db->csyms, (db->ncsyms + 1) * sizeof(cc_csym));
            if (!nc)
                break;
            db->csyms = nc;
            db->csyms[db->ncsyms++] = cs;
        }
    }

    /* Pass 2: C line records (type=1) with spans -> address rows. */
    for (size_t i = 0; i < nlines; i++)
    {
        const char *body;
        if (!rec_is(lines[i], "line", &body))
            continue;
        if (fu32(body, "type", 0) != 1) /* keep only C lines */
            continue;
        const char *sv;
        size_t sn;
        if (!field(body, "span", &sv, &sn))
            continue;
        uint32_t fileid = fu32(body, "file", 0xffffffff);
        int lno = (int)fu32(body, "line", 0);
        const char *fname = (fileid < nfile && files[fileid]) ? files[fileid] : NULL;
        if (!fname || lno <= 0)
            continue;
        /* span is "id" or "id+id+..." */
        const char *p = sv, *end = sv + sn;
        while (p < end)
        {
            uint32_t sid = 0;
            bool any = false;
            while (p < end && *p >= '0' && *p <= '9')
            {
                sid = sid * 10 + (uint32_t)(*p - '0');
                p++;
                any = true;
            }
            if (any && sid < nspan && spans[sid].seg < nseg)
            {
                cc_row *nr = realloc(db->rows, (db->nrows + 1) * sizeof(cc_row));
                if (nr)
                {
                    db->rows = nr;
                    db->rows[db->nrows].addr = segstart[spans[sid].seg] + spans[sid].start;
                    db->rows[db->nrows].size = spans[sid].size ? spans[sid].size : 1;
                    db->rows[db->nrows].file = fname;
                    db->rows[db->nrows].line = lno;
                    db->nrows++;
                }
            }
            if (p < end && *p == '+')
                p++;
            else
                break;
        }
    }

    free(files);
    free(segstart);
    free(segcode);
    free(spans);
    free(symval);
    free(symexp);
    free(symflags);
    free(lines);
    free(buf);

    if (db->nrows == 0)
    {
        cc65dbg_free(db);
        return NULL;
    }
    qsort(db->rows, db->nrows, sizeof(cc_row), row_cmp);
    if (db->nfuncs)
        qsort(db->funcs, db->nfuncs, sizeof(cc_func), func_cmp);
    return db;
}

void cc65dbg_free(cc65dbg_t *db)
{
    if (!db)
        return;
    for (size_t i = 0; i < db->nstrs; i++)
        free(db->strs[i]);
    free(db->strs);
    free(db->rows);
    free(db->funcs);
    free(db->scopes);
    free(db->csyms);
    free(db->segs);
    free(db);
}

/* The linker segments with a name and a non-zero size, at their load addresses. */
int cc65dbg_segments(const cc65dbg_t *db, cc65seg_t *out, int max)
{
    if (!db)
        return 0;
    int n = 0;
    for (size_t i = 0; i < db->nsegs && n < max; i++)
    {
        if (!db->segs[i].name || db->segs[i].size == 0)
            continue;
        out[n].name = db->segs[i].name;
        out[n].addr = (uint16_t)db->segs[i].start;
        out[n].size = db->segs[i].size;
        n++;
    }
    return n;
}

bool cc65dbg_addr_to_src(const cc65dbg_t *db, uint16_t addr, const char **file, int *line)
{
    if (!db || db->nrows == 0)
        return false;
    /* Largest row.addr <= addr, valid only if addr is within that span. */
    size_t lo = 0, hi = db->nrows, best = (size_t)-1;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (db->rows[mid].addr <= addr)
        {
            best = mid;
            lo = mid + 1;
        }
        else
            hi = mid;
    }
    if (best == (size_t)-1)
        return false;
    const cc_row *r = &db->rows[best];
    if (addr >= r->addr + r->size)
        return false;
    if (file)
        *file = r->file;
    if (line)
        *line = r->line;
    return true;
}

bool cc65dbg_src_to_addr(const cc65dbg_t *db, const char *file, int line,
                         uint16_t *addr, int *bound_line)
{
    if (!db || !file)
        return false;
    const char *want = base_name(file);
    bool found = false;
    int best_line = 0;
    uint32_t best_addr = 0;
    for (size_t i = 0; i < db->nrows; i++)
    {
        const cc_row *r = &db->rows[i];
        if (r->line < line || strcmp(base_name(r->file), want) != 0)
            continue;
        if (!found || r->line < best_line || (r->line == best_line && r->addr < best_addr))
        {
            found = true;
            best_line = r->line;
            best_addr = r->addr;
        }
    }
    if (!found)
        return false;
    if (addr)
        *addr = (uint16_t)best_addr;
    if (bound_line)
        *bound_line = best_line;
    return true;
}

const char *cc65dbg_addr_to_func(const cc65dbg_t *db, uint16_t addr)
{
    if (!db || db->nfuncs == 0)
        return NULL;
    size_t lo = 0, hi = db->nfuncs, best = (size_t)-1;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (db->funcs[mid].addr <= addr)
        {
            best = mid;
            lo = mid + 1;
        }
        else
            hi = mid;
    }
    return best == (size_t)-1 ? NULL : db->funcs[best].name;
}

/* True if csym i is an auto whose lexical scope covers pc. */
static bool csym_in_scope(const cc65dbg_t *db, size_t i, uint16_t pc)
{
    const cc_csym *cs = &db->csyms[i];
    if (!cs->is_auto || cs->scope >= db->nscopes)
        return false;
    const cc_scope *s = &db->scopes[cs->scope];
    return s->has && pc >= s->lo && pc < s->hi;
}

int cc65dbg_locals(const cc65dbg_t *db, uint16_t pc,
                   uint8_t (*readmem)(uint16_t addr), cc65var_t *out, int max)
{
    if (!db || !readmem || !db->has_c_sp)
        return 0;
    uint16_t sp = (uint16_t)(readmem(db->c_sp) | (readmem((uint16_t)(db->c_sp + 1)) << 8));

    /* cc65's csym `offs` is relative to the frame base = the C stack pointer ON
     * FUNCTION ENTRY. The prologue (decsp/subysp) then lowers `sp` by the frame
     * size, so at a stop the live `sp` sits frame_size below the frame base and
     * a local's address is `sp + offs + frame_size`. The frame allocated at this
     * PC is the deepest (most negative) auto offset of the in-scope locals. */
    int32_t frame_size = 0;
    for (size_t i = 0; i < db->ncsyms; i++)
        if (csym_in_scope(db, i, pc) && -db->csyms[i].offs > frame_size)
            frame_size = -db->csyms[i].offs;

    int n = 0;
    for (size_t i = 0; i < db->ncsyms && n < max; i++)
    {
        if (!csym_in_scope(db, i, pc))
            continue;
        out[n].name = db->csyms[i].name;
        out[n].addr = (uint16_t)((int32_t)sp + db->csyms[i].offs + frame_size);
        out[n].addr_ok = true;
        n++;
    }
    return n;
}

int cc65dbg_globals(const cc65dbg_t *db, cc65var_t *out, int max)
{
    if (!db)
        return 0;
    int n = 0;
    for (size_t i = 0; i < db->ncsyms && n < max; i++)
    {
        if (!db->csyms[i].is_global)
            continue;
        out[n].name = db->csyms[i].name;
        out[n].addr = (uint16_t)db->csyms[i].addr;
        out[n].addr_ok = true;
        n++;
    }
    return n;
}
