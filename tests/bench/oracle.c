/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "oracle.h"

#include "emu/app/rand.h"
#include "emu/emu/rom.h"
#include "emu/hid/kbd.h"
#include "emu/main.h"
#include "emu/sys/com.h"
#include "emu/sys/cpu.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "emu/sys/vga.h"

#include <string.h>

/* Console capture: everything the machine sends to the terminal, the
 * stream the RTL machine's OS console is compared against. */
static char oracle_tap_buf[65536];
static size_t oracle_tap_len;

static void oracle_tap(const char *buf, int len)
{
    if (len > 0 && oracle_tap_len + (size_t)len <= sizeof(oracle_tap_buf))
    {
        memcpy(oracle_tap_buf + oracle_tap_len, buf, (size_t)len);
        oracle_tap_len += (size_t)len;
    }
}

void oracle_tap_start(void)
{
    oracle_tap_len = 0;
    com_set_tx_tap(oracle_tap);
}

const char *oracle_tap_data(size_t *len)
{
    *len = oracle_tap_len;
    return oracle_tap_buf;
}

/* Sized for the largest canvas; every smaller canvas renders at its own stride. */
static uint32_t oracle_fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

void oracle_init(void)
{
    /* The RTL machine's RAM and XRAM are block RAM and come up zeroed, where
     * the emulator defaults to a random fill because a 6502's SRAM does not.
     * The oracle has to be the same machine, so it takes the FPGA's. */
    mem_set_fill(false, 0, 0);
    main_init();
    vga_set_framebuffer(oracle_fb);
}

bool oracle_restart(const char *rom)
{
    main_stop();
    if (!rom_load(rom))
        return false;
    main_run();
    return true;
}

void oracle_run_frames(int count)
{
    for (int i = 0; i < count; i++)
        sys_run_frame();
}

const uint32_t *oracle_framebuffer(void)
{
    return oracle_fb;
}

void oracle_canvas_size(int *width, int *height)
{
    vga_canvas_size(width, height);
}

unsigned long oracle_frame_count(void)
{
    return sys_frame_count();
}

void oracle_key(uint8_t hid_keycode, bool down)
{
    kbd_hid_set(hid_keycode, down);
}

bool oracle_halted(void)
{
    return cpu_halted();
}

void oracle_seed(uint64_t seed)
{
    rand_set_seed(seed);
}

uint8_t oracle_xram(uint16_t addr)
{
    return xram[addr];
}
