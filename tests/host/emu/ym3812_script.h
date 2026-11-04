/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Register-write scripts for the YM3812 tests, and the reference for the
 * engine's write pacing. */

#ifndef _TESTS_HOST_EMU_YM3812_SCRIPT_H_
#define _TESTS_HOST_EMU_YM3812_SCRIPT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* n is the sample the write is pushed before, or in a schedule the sample it
 * is applied before. */
typedef struct
{
    uint32_t n;
    uint8_t reg, val;
} ym_write_t;

typedef struct
{
    ym_write_t *w;
    size_t count, cap;
    uint32_t samples;
} ym_script_t;

/* Where a pushed write went: the sample it was applied before, or one of
 * these. */
#define YM_SKIPPED UINT32_MAX
#define YM_DROPPED (UINT32_MAX - 1)
#define YM_WAITING (UINT32_MAX - 2)

void ym_script_free(ym_script_t *s);
void ym_script_add(ym_script_t *s, uint32_t n, uint8_t reg, uint8_t val);

/* Offsets the engine queues: 01, 08, BD, the operator and channel registers
 * and the channel aliases. */
bool ym_is_reg(uint8_t reg);

/* The pacing rule, written from its statement: at[i] for every push, and the
 * applied writes in order as a schedule. Returns the number dropped. */
uint32_t ym_pace(const ym_script_t *pushes, uint32_t *at, ym_script_t *schedule);

/* Scripts the die can take on its bus: each write applied 2 samples or more
 * after the one before, or in ym_script_dense at the first sample the bus
 * allows, which is the next one for operator to channel, operator to global,
 * channel to global and global to global. Every script starts with IC
 * released at sample 0 and a test-register preamble. */
void ym_script_random(ym_script_t *s, uint32_t seed, uint32_t samples, unsigned flags);
#define YM_RHYTHM 0x01    /* rhythm mode and drum keys */
#define YM_ALIAS 0x02     /* channel aliases, holes and timer registers */
#define YM_SLOW_ENV 0x04  /* low envelope rates */
#define YM_TEST_BITS 0x08 /* register 01 bits 3 and 7; dense: one-sample pulses */
#define YM_GAP2 0x10      /* dense writes 2 samples apart or more */
void ym_script_dense(ym_script_t *s, uint32_t seed, uint32_t samples, unsigned flags);
void ym_script_power_on(ym_script_t *s);
void ym_script_envelopes(ym_script_t *s);
void ym_script_levels(ym_script_t *s);
void ym_script_waves(ym_script_t *s);
void ym_script_feedback(ym_script_t *s);
void ym_script_lfo(ym_script_t *s);
void ym_script_rhythm(ym_script_t *s);
void ym_script_retrigger(ym_script_t *s);
void ym_script_clip(ym_script_t *s, bool rhythm);
void ym_script_test_bits(ym_script_t *s, uint32_t seed);
void ym_script_eg_wrap(ym_script_t *s);

/* src with every offset the engine skips written, random data, wherever the
 * bus has room between two writes, and 08 bit 7 cleared so that a timer the
 * holes start never keys a slot (spec 8.4). */
void ym_script_holes(ym_script_t *s, const ym_script_t *src, uint32_t seed);

/* Scripts with bursts: many writes pushed before one sample. */
void ym_script_bursts(ym_script_t *s, uint32_t seed, uint32_t samples, unsigned flags);
void ym_script_flood(ym_script_t *s, uint32_t seed);

#endif /* _TESTS_HOST_EMU_YM3812_SCRIPT_H_ */
