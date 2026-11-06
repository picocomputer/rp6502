/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "psg_shim.h"

#include "core/aud/mix.h"
#include "core/aud/psg.h"
#include "core/aud/sine.h"

#include "aud_sine_tables.h"

int16_t sine_table[256];
uint16_t sine_pan[127];

void aud_setup_probe(void (*sample)(int16_t *left, int16_t *right)) { (void)sample; }
void aud_psg_map(uint16_t word) { psg_map(word); }

void shim_init(void)
{
    for (int i = 0; i < 256; i++)
        sine_table[i] = AUD_SINE_TABLE[i];
    for (int i = 0; i < 127; i++)
        sine_pan[i] = AUD_PAN_TABLE[i];
}

void shim_sample(int16_t *l, int16_t *r) { psg_sample(l, r); }

void shim_xram_write(uint16_t addr, uint8_t val) { psg_xram_write(addr, val); }
