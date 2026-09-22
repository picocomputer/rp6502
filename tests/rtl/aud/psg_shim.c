/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "psg_shim.h"

#include "core/aud/mix.h"
#include "core/aud/psg.h"
#include "core/aud/sine.h"
#include "core/sys/ria.h"
#include "core/sys/xram.h"

#include "aud_sine_tables.h"

static uint8_t xram_backing[0x10000];
volatile uint8_t *const xram = xram_backing;
static uint16_t shim_watch = 0xFFFF;

int16_t sine_table[256];

void aud_setup(aud_dev_t dev) { (void)dev; }
void opl_park(void) {}
aud_dev_t aud_device(void) { return aud_dev_none; }
void aud_setup_probe(void (*sample)(int16_t *left, int16_t *right)) { (void)sample; }
void aud_stop(void) {}
void aud_engine_lock(void) {}
void aud_engine_unlock(void) {}

void ria_aud_watch(uint16_t xaddr)
{
    shim_watch = xaddr == 0xFFFF ? 0xFFFF : (uint16_t)(xaddr & 0xFF00);
}

void shim_init(void)
{
    for (int i = 0; i < 256; i++)
        sine_table[i] = AUD_SINE_TABLE[i];
}

void shim_sample(int16_t *l, int16_t *r)
{
    psg_sample(l, r);
}

void shim_xram_write(uint16_t addr, uint8_t val)
{
    xram_backing[addr] = val;
    if ((addr & 0xFF00) == shim_watch)
        psg_xram_write((uint8_t)addr, val);
}

uint8_t shim_xram_read(uint16_t addr)
{
    return xram_backing[addr];
}
