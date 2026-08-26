/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What the HID drivers reach for, on a host that is none of their
 * platforms. The Pocket answers these in rtl/sw/hid.c and the RIA in
 * its own drivers; here they only have to exist and be quiet, because
 * what test_apf.c asks about is where the bits landed.
 */

#include "core/api/oem.h"
#include "core/cfg.h"
#include "core/hid/hid.h"
#include "core/main.h"
#include "core/vga/vga.h"
#include "host/os.h"

#include <stdint.h>

static uint8_t apf_host_xram[0x10000];
volatile uint8_t *const xram = apf_host_xram;

bool hid_boot_enumerating(void)
{
    return false;
}

void hid_set_leds(uint8_t leds)
{
    (void)leds;
}

/* CP437 to start, which is what every layout's code points are converted
 * through unless something says otherwise. Settable because the real machine's
 * is: a 6502 program can move it through ATTR_CODE_PAGE, and what that does to
 * the layout is worth being able to ask. */
static uint16_t apf_host_code_page = 437;

uint16_t oem_get_code_page_run(void)
{
    return apf_host_code_page;
}

void oem_set_code_page_run(uint16_t cp)
{
    apf_host_code_page = cp;
}

/* The console canvas, so an absolute pointer has a space to be in. */
void vga_canvas_size(int *w, int *h)
{
    *w = 640;
    *h = 480;
}

void ria_trigger_sigint(void)
{
}

/* The machine clock the key repeat asks for. Standing still: a repeat is
 * a thing that happens to a key still held later, and nothing here holds
 * one. */
uint64_t host_clock_us(void)
{
    return 0;
}

void cfg_save(void)
{
}

bool main_break(void)
{
    return false;
}

bool main_break_to_launcher(void)
{
    return false;
}
