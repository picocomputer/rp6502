/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/str/oem.h"
#include "core/hid/hid.h"
#include "core/api/proc.h"
#include "core/sys/sys.h"
#include "core/term/cells.h"
#include "core/vga/vga.h"
#include "osal/os.h"

#include <stdint.h>

volatile uint8_t xram[0x10000];

/* The console's terminal, whose key modes the test sets by hand. */
term_t cells_term = {.autorepeat = true};

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

bool oem_set_code_page_run(uint16_t cp)
{
    apf_host_code_page = cp;
    return true;
}

void vga_canvas_size(int *w, int *h)
{
    *w = 640;
    *h = 480;
}

void vga_display_size(int *w, int *h)
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

/* The clock auto-repeat runs on, which the test moves. */
uint64_t apf_host_ns;

uint64_t os_mono_ns(void)
{
    return apf_host_ns;
}

/* Ctrl-Alt-Delete, counted in place of breaking to the monitor. */
int apf_host_chords;

bool proc_key_chord(uint8_t keycode, bool ctrl, bool alt)
{
    if ((keycode != 0x4C && keycode != 0x63) || !ctrl || !alt)
        return false;
    apf_host_chords++;
    return true;
}
