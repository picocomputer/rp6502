/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_AUD_PSG_SHIM_H_
#define _TESTS_AUD_PSG_SHIM_H_

#include <stdint.h>

/* The rate aud_psg.sv's arithmetic is elaborated for. test_psg hands the
 * same number to psg_setup and to the model's RATE parameter, because a
 * lockstep between two engines built for different rates compares nothing —
 * and psg_setup divides by it, so leaving it unset is a division by zero
 * rather than a quiet mismatch. Not shim_init's job: test_bel links this
 * shim without psg.c.
 *
 * Distinct from TICKS_PER_SAMPLE, which the test shortens so the simulation
 * runs faster. The tick decides how often a sample happens; this decides
 * what the sample is. */
#define PSG_SHIM_RATE 48000

#ifdef __cplusplus
extern "C"
{
#endif

    void shim_init(void);
    void shim_sample(int16_t *l, int16_t *r);
    void shim_xram_write(uint16_t addr, uint8_t val);
    uint8_t shim_xram_read(uint16_t addr);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_AUD_PSG_SHIM_H_ */
