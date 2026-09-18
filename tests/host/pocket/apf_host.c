/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/str/oem.h"
#include "core/hid/hid.h"
#include "core/sys/sys.h"
#include "core/vga/vga.h"
#include "osal/os.h"

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

static uint16_t apf_host_code_page = 437;

uint16_t oem_get_code_page_run(void)
{
    return apf_host_code_page;
}

void oem_set_code_page_run(uint16_t cp)
{
    apf_host_code_page = cp;
}

void vga_canvas_size(int *w, int *h)
{
    *w = 640;
    *h = 480;
}

void ria_trigger_sigint(void)
{
}

uint64_t host_clock_us(void)
{
    return 0;
}

uint64_t os_mono_ns(void)
{
    return 0;
}


bool sys_break(void)
{
    return false;
}

bool sys_break_to_launcher(void)
{
    return false;
}
