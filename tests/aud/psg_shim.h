/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_AUD_PSG_SHIM_H_
#define _TESTS_AUD_PSG_SHIM_H_

#include <stdint.h>

/* The rate aud_psg.sv's constants are elaborated for: its envelope tables
 * divide by `24 * ms` and its phase divider by 72000. test_psg hands the
 * same number to psg_setup, because a lockstep between two engines built
 * for different rates compares nothing — and psg_setup divides by it, so
 * leaving it unset is a division by zero rather than a quiet mismatch.
 * Not shim_init's job: test_bel links this shim without psg.c. Move this
 * when aud_psg.sv's constants move. */
#define PSG_SHIM_RATE 24000

#ifdef __cplusplus
extern "C"
{
#endif

    void shim_init(void);
    void shim_sample(int16_t *l, int16_t *r);
    void shim_xram_write(uint16_t addr, uint8_t val);
    uint32_t shim_xram_word(uint16_t word_addr);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_AUD_PSG_SHIM_H_ */
