/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vec.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define VEC_MAX_RAM 64
#define VEC_MAX_CYCLES 16

static uint8_t vec_mem[0x10000];

typedef struct
{
    dut_regs_t regs;
    uint16_t addr[VEC_MAX_RAM];
    uint8_t val[VEC_MAX_RAM];
    uint16_t count;
} vec_state_t;

typedef struct
{
    uint16_t addr;
    uint8_t val;
    uint8_t read;
} vec_cycle_t;

static bool rd(FILE *f, void *dst, size_t n)
{
    return fread(dst, 1, n, f) == n;
}

static bool rd_u8(FILE *f, uint8_t *v) { return rd(f, v, 1); }

static bool rd_u16(FILE *f, uint16_t *v)
{
    uint8_t b[2];
    if (!rd(f, b, 2))
        return false;
    *v = (uint16_t)(b[0] | (b[1] << 8));
    return true;
}

static bool rd_state(FILE *f, vec_state_t *s)
{
    if (!rd_u16(f, &s->regs.pc) || !rd_u8(f, &s->regs.s) || !rd_u8(f, &s->regs.a) ||
        !rd_u8(f, &s->regs.x) || !rd_u8(f, &s->regs.y) || !rd_u8(f, &s->regs.p) ||
        !rd_u16(f, &s->count) || s->count > VEC_MAX_RAM)
        return false;
    for (uint16_t i = 0; i < s->count; i++)
        if (!rd_u16(f, &s->addr[i]) || !rd_u8(f, &s->val[i]))
            return false;
    return true;
}

static void fail(vec_result_t *r, uint8_t opcode, size_t index, const char *fmt, ...)
{
    r->failed++;
    if (r->detail[0])
        return;
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(r->detail, sizeof r->detail, "opcode $%02X test %zu: %s",
             opcode, index, msg);
}

bool vec_run(const char *path, const dut_t *cpu, int only_opcode,
             vec_result_t *result)
{
    memset(result, 0, sizeof *result);

    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    char magic[4];
    uint32_t ntests;
    uint8_t nb[4];
    if (!rd(f, magic, 4) || memcmp(magic, "SSV1", 4) != 0 || !rd(f, nb, 4))
    {
        fclose(f);
        return false;
    }
    ntests = (uint32_t)nb[0] | ((uint32_t)nb[1] << 8) |
             ((uint32_t)nb[2] << 16) | ((uint32_t)nb[3] << 24);

    for (uint32_t t = 0; t < ntests; t++)
    {
        uint8_t opcode, ncyc;
        vec_state_t initial, final;
        vec_cycle_t cycles[VEC_MAX_CYCLES];

        if (!rd_u8(f, &opcode) || !rd_state(f, &initial) || !rd_state(f, &final) ||
            !rd_u8(f, &ncyc) || ncyc > VEC_MAX_CYCLES)
        {
            fclose(f);
            return false;
        }
        for (uint8_t i = 0; i < ncyc; i++)
            if (!rd_u16(f, &cycles[i].addr) || !rd_u8(f, &cycles[i].val) ||
                !rd_u8(f, &cycles[i].read))
            {
                fclose(f);
                return false;
            }

        if (only_opcode >= 0 && opcode != (uint8_t)only_opcode)
            continue;

        /* Zero first: final lists stack bytes that initial never mentions. */
        memset(vec_mem, 0, sizeof vec_mem);
        for (uint16_t i = 0; i < initial.count; i++)
            vec_mem[initial.addr[i]] = initial.val[i];

        cpu->begin(&initial.regs);

        bool bad = false;
        for (uint8_t i = 0; i < ncyc && !bad; i++)
        {
            uint16_t addr;
            bool read;
            bool sync;
            cpu->bus(&addr, &read, &sync);

            uint8_t data = vec_mem[addr];
            cpu->tick(&data);
            if (!read)
                vec_mem[addr] = data;

            if (addr != cycles[i].addr || data != cycles[i].val ||
                (uint8_t)read != cycles[i].read)
            {
                fail(result, opcode, t, "cycle %u was %04X %02X %s, want %04X %02X %s",
                     i, addr, data, read ? "read" : "write",
                     cycles[i].addr, cycles[i].val, cycles[i].read ? "read" : "write");
                bad = true;
            }
        }

        if (!bad)
        {
            dut_regs_t got;
            cpu->end(&got);
            if (got.pc != final.regs.pc || got.s != final.regs.s ||
                got.a != final.regs.a || got.x != final.regs.x ||
                got.y != final.regs.y || got.p != final.regs.p)
            {
                fail(result, opcode, t,
                     "regs were pc=%04X s=%02X a=%02X x=%02X y=%02X p=%02X, "
                     "want pc=%04X s=%02X a=%02X x=%02X y=%02X p=%02X",
                     got.pc, got.s, got.a, got.x, got.y, got.p,
                     final.regs.pc, final.regs.s, final.regs.a,
                     final.regs.x, final.regs.y, final.regs.p);
                bad = true;
            }
            for (uint16_t i = 0; i < final.count && !bad; i++)
                if (vec_mem[final.addr[i]] != final.val[i])
                {
                    fail(result, opcode, t, "ram $%04X was %02X, want %02X",
                         final.addr[i], vec_mem[final.addr[i]], final.val[i]);
                    bad = true;
                }
        }

        if (!bad)
            result->passed++;
    }

    fclose(f);
    return true;
}
