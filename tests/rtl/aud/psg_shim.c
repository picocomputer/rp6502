/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "psg_shim.h"

#include "core/aud/mix.h"
#include "core/aud/psg.h"
#include "core/aud/sine.h"
#include "core/sys/xram.h"

#include "aud_sine_tables.h"

static uint8_t xram_backing[0x10000];
volatile uint8_t *const xram = xram_backing;

int16_t sine_table[256];

void aud_setup_probe(void (*sample)(int16_t *left, int16_t *right)) { (void)sample; }
void aud_psg_map(uint16_t word) { psg_map(word); }

void shim_init(void)
{
    for (int i = 0; i < 256; i++)
        sine_table[i] = AUD_SINE_TABLE[i];
}

void shim_sample(int16_t *l, int16_t *r)
{
    psg_sample(&xram_backing[psg_xaddr_get()], l, r);
}

void shim_xram_write(uint16_t addr, uint8_t val)
{
    xram_backing[addr] = val;
    uint16_t psg = psg_xaddr_get();
    if (psg != 0xFFFF && (addr & 0xFF00) == (psg & 0xFF00))
        psg_xram_write((uint8_t)addr, val);
}

uint8_t shim_xram_read(uint16_t addr)
{
    return xram_backing[addr];
}
