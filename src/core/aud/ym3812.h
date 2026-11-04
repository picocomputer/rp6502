/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_YM3812_H_
#define _CORE_AUD_YM3812_H_

/* A YM3812 (OPL2), one chip sample per ym3812_sample, bit-exact with the die
 * at its output word.
 *
 * Writes wait in a queue and apply in order, at most one a sample and one per
 * 96 master clocks. Before each sample the oldest write applies if the credit
 * is at least 96 and pays 96, unless the sample applies a late half: the
 * modulator half of a write to A0-A2, B0-B2, C0-C2, AE, AF, BE, BF, CE or CF
 * (channels 0-2) applied before the previous sample. Then the credit gains 72
 * and stops at 192 while writes wait, 96 while none do (spec 7.8). A write to
 * an offset that is not a register the engine implements never enters the
 * queue; one that finds the queue full is dropped and counted. Timers (02-04),
 * CSM (08 bit 7), status and test bits 0 and 2 are not implemented. */

#include "core/sys/sst.h"
#include <stdbool.h>
#include <stdint.h>

#define YM3812_QUEUE_SIZE 256

/* Entries are reg | val << 8, the oldest at head. */
typedef struct
{
    uint16_t entry[YM3812_QUEUE_SIZE];
    uint16_t count;
    uint8_t head;
    uint32_t drops;
} ym3812_queue_t;

extern ym3812_queue_t ym3812_queue;

/* Bit reg & 7 of byte reg >> 3 is set for the offsets the engine acts on. */
extern const uint8_t ym3812_regs[32];

static inline void ym3812_push(uint8_t reg, uint8_t val)
{
    ym3812_queue_t *q = &ym3812_queue;
    const uint32_t n = q->count;
    if (!((ym3812_regs[reg >> 3] >> (reg & 7)) & 1))
        return;
    if (n == YM3812_QUEUE_SIZE)
    {
        if (q->drops < 0x7FFFFFFF)
            q->drops++;
        return;
    }
    q->entry[(uint8_t)(q->head + n)] = (uint16_t)(reg | val << 8);
    q->count = (uint16_t)(n + 1);
}

/* Power-on state, an empty queue, the credit at 96 and no drops. */
void ym3812_reset(void);

/* Applies at most one queued write, renders one chip sample and returns the
 * YM3014B word. */
int16_t ym3812_sample(void);

/* Writes dropped since the last call, saturating at 0x7FFFFFFF. */
uint32_t ym3812_take_drops(void);

/* 18 slots of 12, 9 channels of 10, 33 of globals and counters, then the
 * queue: its count, 256 entries of 2, the credit and the drop count. A load
 * that refuses the bytes leaves the engine reset. */
#define YM3812_SST_SIZE (18 * 12 + 9 * 10 + 33 + 2 + YM3812_QUEUE_SIZE * 2 + 1 + 4)
void ym3812_sst_save(sst_cursor_t *c);
bool ym3812_sst_load(sst_cursor_t *c);

#endif /* _CORE_AUD_YM3812_H_ */
