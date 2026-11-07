/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_OPL_H_
#define _CORE_AUD_OPL_H_

/* A YM3812 (OPL2), one chip sample per opl_sample, bit-exact with the die
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

#include "core/sys/ring.h"
#include "core/sys/sst.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define OPL_QUEUE_SIZE 256

/* The 256 bytes whose byte n is register n. */
#define OPL_REGS_SIZE 256

/* A block may start anywhere it ends inside XRAM. */
static inline bool opl_block_ok(uint16_t word)
{
    return word <= 0x10000 - OPL_REGS_SIZE;
}

/* Entries are reg | val << 8. */
typedef struct
{
    RING(uint16_t, OPL_QUEUE_SIZE) ring;
    uint32_t drops;
} opl_queue_t;

extern opl_queue_t opl_queue;

/* Bit reg & 7 of byte reg >> 3 is set for the offsets the engine acts on. */
extern const uint8_t opl_regs[32];

static inline bool opl_is_reg(uint8_t reg)
{
    return (opl_regs[reg >> 3] >> (reg & 7)) & 1;
}

/* Whether applying a write to reg holds the queue for a sample: the
 * modulator half of A0-A2, B0-B2, C0-C2, AE, AF, BE, BF, CE or CF. */
static inline bool opl_is_late(uint8_t reg)
{
    const uint8_t g = reg >> 4, lo = reg & 15;
    return g >= 0xA && g <= 0xC && (lo <= 2 || lo >= 14);
}

/* A write to byte reg of the engine's 256, queued for opl_sample. */
static inline void opl_push(uint8_t reg, uint8_t val)
{
    opl_queue_t *q = &opl_queue;
    if (!opl_is_reg(reg))
        return;
    if (!RING_PUT(&q->ring, (uint16_t)(reg | val << 8)) && q->drops < 0x7FFFFFFF)
        q->drops++;
}

/* n writes of reg | val << 8, each to a register opl_is_reg accepts, queued
 * in order for opl_sample. Those that find the queue full are dropped and
 * counted. */
static inline void opl_push_n(const uint16_t *w, uint32_t n)
{
    opl_queue_t *q = &opl_queue;
    const uint32_t lost = n - RING_IN(&q->ring, w, n);
    q->drops = lost > 0x7FFFFFFF - q->drops ? 0x7FFFFFFF : q->drops + lost;
}

/* Power-on state, an empty queue, the credit at 96 and no drops. */
void opl_reset(void);

/* Applies at most one queued write, renders one chip sample and returns the
 * YM3014B word. */
int16_t opl_sample(void);

/* Writes dropped since the last call, saturating at 0x7FFFFFFF. */
uint32_t opl_take_drops(void);

/* The pacing credit, and whether the next sample is held for a late half. */
void opl_pace_get(uint32_t *credit, bool *late);

/* Resets the engine and maps it at word, 0xFFFF for parked. A software
 * machine reaches it through aud_opl_map. */
void opl_map(uint16_t word);

/* Where this engine's 256 bytes start in XRAM, 0xFFFF for parked. */
uint16_t opl_xaddr_get(void);

/* Maps the engine at word without resetting it. The Pico resets the engine
 * while it is parked, outside its engine lock, then maps it with this under
 * the lock. */
void opl_xaddr_set(uint16_t word);

/* Release the pointer, which takes the OPL2 out of the mix, and drop the
 * writes still queued. The chip keeps its state until the next map resets
 * it. The mixer calls it with its engines held still. */
void opl_park(void);

/* The pointer, 18 slots of 12, 9 channels of 10, 33 of globals and counters,
 * then the queue: its count, 256 entries of 2, the credit and the drop count.
 * A load that refuses the bytes leaves the engine reset. */
#define OPL_SST_SIZE (2 + 18 * 12 + 9 * 10 + 33 + 2 + OPL_QUEUE_SIZE * 2 + 1 + 4)
void opl_sst_save(sst_cursor_t *c, unsigned flags);
bool opl_sst_load(sst_cursor_t *c, unsigned flags);

#define OPL_DRIVER DRIVER(opl_reset, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(OPL_, 2, OPL_SST_SIZE, opl_sst_save, opl_sst_load))

#endif /* _CORE_AUD_OPL_H_ */
