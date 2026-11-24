/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/api.h"
#include "core/str/oem.h"
#include "core/wdc/phi2.h"
#include "core/com/rln.h"
#include "core/hid/clip.h"
#include "core/hid/hid.h"
#include "core/hid/paste.h"
#include "core/hid/tablet.h"
#include "core/api/proc.h"
#include "core/sys/sys.h"
#include "core/term/vterm.h"
#include "core/vga/vga.h"
#include "osal/os.h"

#include <stdint.h>

volatile uint8_t xram[0x10000];

/* The console's terminal, whose key modes the test sets by hand. */
term_t vterm = {.autorepeat = true};

bool hid_boot_enumerating(void)
{
    return false;
}

/* What the sources linked here name and the test never reaches: the
 * settings beside the keyboard's in config.c, and the errno of an API
 * call. */
bool phi2_check_khz(uint16_t *v)
{
    (void)v;
    return false;
}

void phi2_apply_khz(uint16_t phi2_khz, bool changed)
{
    (void)phi2_khz;
    (void)changed;
}

void oem_locale_changed(uint16_t cp)
{
    (void)cp;
}

uint16_t api_platform_errno(api_errno num)
{
    return (uint16_t)num;
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

/* The copy and paste the keys reach, the line editor's and the console's,
 * and the console's mouse, which owns the tablet. The test presses none of
 * their keys and moves no pointer. */
bool rln_selected(void)
{
    return false;
}

bool rln_copy(void)
{
    return false;
}

bool rln_cut(void)
{
    return false;
}

bool rln_paste(void)
{
    return false;
}

void clip_copy_primary(void)
{
}

void clip_paste(uint8_t which)
{
    (void)which;
}

void paste_cut(void)
{
}

void paste_cancel(void)
{
}

void tablet_on_contact(uint8_t old_flags, uint8_t flags, int16_t x, int16_t y,
                       int8_t dwheel, int8_t dpan, bool moved_by_user)
{
    (void)old_flags, (void)flags, (void)x, (void)y;
    (void)dwheel, (void)dpan, (void)moved_by_user;
}

bool tablet_claimed(void)
{
    return false;
}

bool tablet_wheel_claimed(void)
{
    return false;
}

void vga_set_pointer(uint8_t shape, int16_t x, int16_t y)
{
    (void)shape, (void)x, (void)y;
}
