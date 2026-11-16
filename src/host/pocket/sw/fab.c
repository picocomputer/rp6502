/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "fab.h"
#include "main.h"

#include "host/host.h"

#define FAB_REGS_RUN(at, n) {at, n},
static const struct
{
    uint8_t at, n;
} fab_regs[] = {FAB_REGS_RUNS(FAB_REGS_RUN)};

#define FAB_REG(w) (((volatile uint32_t *)REGS_WIN)[w])

/* The ST_SEL pick of each JAM_DATA word. */
static uint32_t fab_pick(int k)
{
    if (k < JAM_VIA)
        return ST_PICK(ST_CPU, k);
    if (k < JAM_MACH)
        return ST_PICK(ST_VIA, k - JAM_VIA);
    if (k < JAM_BUS)
        return ST_PICK(ST_MACH, k - JAM_MACH);
    return ST_PICK(ST_BUS, k - JAM_BUS);
}

/* SRAM and XRAM_WIN answer a word access with one byte, so they are read
 * and written a byte at a time. Their bytes are in the blob in order, and
 * go to and from it a word at a time when the blob is aligned there. */
static void fab_put_win(sst_cursor_t *c, volatile const uint8_t *win)
{
    uint8_t *p = sst_room(c, 0x10000);
    if (!p)
        return;
    if ((uintptr_t)p & 3u)
    {
        for (uint32_t i = 0; i < 0x10000; i++)
            p[i] = win[i];
        return;
    }
    sst_word_t *w = (sst_word_t *)p;
    for (uint32_t i = 0; i < 0x10000; i += 4)
        *w++ = win[i] | (uint32_t)win[i + 1] << 8
               | (uint32_t)win[i + 2] << 16 | (uint32_t)win[i + 3] << 24;
}

static void fab_get_win(const uint8_t *p, volatile uint8_t *win)
{
    if ((uintptr_t)p & 3u)
    {
        for (uint32_t i = 0; i < 0x10000; i++)
            win[i] = p[i];
        return;
    }
    const sst_word_t *w = (const sst_word_t *)p;
    for (uint32_t i = 0; i < 0x10000; i += 4)
    {
        uint32_t v = *w++;
        win[i] = (uint8_t)v;
        win[i + 1] = (uint8_t)(v >> 8);
        win[i + 2] = (uint8_t)(v >> 16);
        win[i + 3] = (uint8_t)(v >> 24);
    }
}

void fab_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u64(c, host_clock_us());
    sst_put_u32(c, VID_FRAME);
    for (int k = 0; k < FAB_JAM_WORDS; k++)
    {
        ST_SEL = fab_pick(k);
        sst_put_u32(c, ST_DATA);
    }
    for (unsigned r = 0; r < sizeof fab_regs / sizeof fab_regs[0]; r++)
        for (unsigned w = fab_regs[r].at; w < fab_regs[r].at + fab_regs[r].n; w++)
            sst_put_u32(c, FAB_REG(w));
    uint8_t *x = sst_room(c, 4 * FAB_XPROG_WORDS);
    if (x)
        for (uint32_t line = 0; line < VGA_PROG_MAX; line++)
            for (uint32_t plane = 0; plane < SCANVIDEO_PLANE_COUNT; plane++)
                for (uint32_t w = 0; w < VID_XPROG_WORDS; w++, x += 4)
                    sst_be32_write(x, VID_XPROG(line, plane, w));
    fab_put_win(c, SRAM);
    fab_put_win(c, XRAM_WIN);
}

static inline uint32_t fab_u32(const uint8_t **p)
{
    uint32_t v = sst_be32_read(*p);
    *p += 4;
    return v;
}

/* Everything the 6502 runs on goes back before the 6502 itself: the
 * memories, then the RIA, then RESB and the jam, which loads the 6502, the
 * VIA, the PHI2 count and the open bus on one clock. RESB is written as it
 * was saved, and not through resb_release, which would clear the
 * interrupt word put back above. */
bool fab_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    const uint8_t *p = sst_view(c, FAB_SST_SIZE);
    if (!p)
        return sst_refuse(c, "the fabric's state is short");
    uint64_t mtime = (uint64_t)fab_u32(&p) << 32;
    mtime |= fab_u32(&p);
    tim_clock_set(mtime);
    VID_FRAME = fab_u32(&p);
    uint32_t jam[FAB_JAM_WORDS];
    for (int k = 0; k < FAB_JAM_WORDS; k++)
        jam[k] = fab_u32(&p);
    const uint8_t *regs_at = p;
    p += 4 * FAB_REGS_WORDS;
    for (uint32_t line = 0; line < VGA_PROG_MAX; line++)
        for (uint32_t plane = 0; plane < SCANVIDEO_PLANE_COUNT; plane++)
            for (uint32_t w = 0; w < VID_XPROG_WORDS; w++)
                VID_XPROG(line, plane, w) = fab_u32(&p);
    fab_get_win(p, SRAM);
    fab_get_win(p + 0x10000, XRAM_WIN);

    p = regs_at;
    REGS_RESTORE = 1;
    for (unsigned r = 0; r < sizeof fab_regs / sizeof fab_regs[0]; r++)
        for (unsigned w = fab_regs[r].at; w < fab_regs[r].at + fab_regs[r].n; w++)
            FAB_REG(w) = fab_u32(&p);
    REGS_RESTORE = 0;

    CPU_RESB = (uint8_t)(jam[JAM_MACH] & 1);
    for (int k = 0; k < FAB_JAM_WORDS; k++)
        JAM_DATA(k) = jam[k];
    JAM = 1;
    return true;
}
