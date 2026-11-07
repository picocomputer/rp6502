/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Register-write scripts for the YM3812 tests, and the reference for the
 * engine's write pacing. */

#ifndef _TESTS_HOST_EMU_OPL_SCRIPT_H_
#define _TESTS_HOST_EMU_OPL_SCRIPT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* n is the sample the write is pushed before, or in a schedule the sample it
 * is applied before. */
typedef struct
{
    uint32_t n;
    uint8_t reg, val;
} opl_write_t;

typedef struct
{
    opl_write_t *w;
    size_t count, cap;
    uint32_t samples;
} opl_script_t;

/* Where a pushed write went: the sample it was applied before, or one of
 * these. */
#define OPL_SKIPPED UINT32_MAX
#define OPL_DROPPED (UINT32_MAX - 1)
#define OPL_WAITING (UINT32_MAX - 2)

void opl_script_free(opl_script_t *s);
void opl_script_add(opl_script_t *s, uint32_t n, uint8_t reg, uint8_t val);

/* Offsets the engine queues: 01, 08, BD, the operator and channel registers
 * and the channel aliases. */
bool opl_is_reg(uint8_t reg);

/* The pacing rule, written from its statement: at[i] for every push, and the
 * applied writes in order as a schedule. Returns the number dropped. */
uint32_t opl_pace(const opl_script_t *pushes, uint32_t *at, opl_script_t *schedule);

/* Scripts the die can take on its bus: each write applied 2 samples or more
 * after the one before, or in opl_script_dense at the first sample the bus
 * allows, which is the next one for operator to channel, operator to global,
 * channel to global and global to global. Every script starts with IC
 * released at sample 0 and a test-register preamble. */
void opl_script_random(opl_script_t *s, uint32_t seed, uint32_t samples, unsigned flags);
#define OPL_RHYTHM 0x01    /* rhythm mode and drum keys */
#define OPL_ALIAS 0x02     /* channel aliases, holes and timer registers */
#define OPL_SLOW_ENV 0x04  /* low envelope rates */
#define OPL_TEST_BITS 0x08 /* register 01 bits 3 and 7; dense: one-sample pulses */
#define OPL_GAP2 0x10      /* dense writes 2 samples apart or more */
void opl_script_dense(opl_script_t *s, uint32_t seed, uint32_t samples, unsigned flags);
void opl_script_power_on(opl_script_t *s);
void opl_script_envelopes(opl_script_t *s);
void opl_script_levels(opl_script_t *s);
void opl_script_waves(opl_script_t *s);
void opl_script_feedback(opl_script_t *s);
void opl_script_lfo(opl_script_t *s);
void opl_script_rhythm(opl_script_t *s);
void opl_script_retrigger(opl_script_t *s);
void opl_script_clip(opl_script_t *s, bool rhythm);
void opl_script_test_bits(opl_script_t *s, uint32_t seed);
void opl_script_eg_wrap(opl_script_t *s);
void opl_script_keyoff_silent(opl_script_t *s);
void opl_script_decay_sl0(opl_script_t *s);

/* src with every offset the engine skips written, random data, wherever the
 * bus has room between two writes, and 08 bit 7 cleared so that a timer the
 * holes start never keys a slot (spec 8.4). */
void opl_script_holes(opl_script_t *s, const opl_script_t *src, uint32_t seed);

/* Scripts with bursts: many writes pushed before one sample. */
void opl_script_bursts(opl_script_t *s, uint32_t seed, uint32_t samples, unsigned flags);
void opl_script_flood(opl_script_t *s, uint32_t seed);

#endif /* _TESTS_HOST_EMU_OPL_SCRIPT_H_ */
