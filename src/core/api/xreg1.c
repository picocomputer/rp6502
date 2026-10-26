/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Device 1 of the XREG space, the video device.
 *
 * Only a machine that renders its own video can link this file, because it
 * calls into core/vga. A machine whose video device is a real chip across the
 * PIX bus answers device 1 at the far end (host/pico/vga/sys/pix.c) and lists
 * core/api/xreg0.c alone.
 */

#include "core/api/xreg.h"
#include "core/sys/driver.h"
#include "core/vga/vga.h"

#include <string.h>

/* The mode program being assembled. Channel 0 stores each register as it
 * arrives and the mode write at index 1 consumes the lot, which works
 * because core/sys/pix.c delivers the registers after MODE before MODE. */
static uint16_t xregs[16];

void xreg1_clear(void)
{
    memset(xregs, 0, sizeof(xregs));
}

void xreg_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    for (int i = 0; i < 16; i++)
        sst_put_u16(c, xregs[i]);
}

bool xreg_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint16_t in[16];
    for (int i = 0; i < 16; i++)
        in[i] = sst_get_u16(c);
    if (!sst_ok(c))
        return false;
    memcpy(xregs, in, sizeof xregs);
    return true;
}

bool xreg1(uint8_t channel, uint8_t index, uint16_t word)
{
    if (channel == 0)
    {
        xregs[index & 0x0F] = word;
        if (index == 0) /* CANVAS */
        {
            bool ok = vga_canvas_select(word);
            xreg1_clear();
            return ok;
        }
        if (index == 1) /* MODE */
        {
            /* The console canvas is not programmable. */
            if (vga_canvas_is_console())
            {
                xreg1_clear();
                return false;
            }
            vga_mode_begin((uint8_t)word, xregs[2]);
            bool ok = vga_mode_prog(word, xregs);
            xreg1_clear();
            return ok;
        }
        return true;
    }
    if (channel == 0x0F)
    {
        /* pix_api_xreg refuses a program's write to this channel, so what
         * arrives here comes only from the machine itself. */
        if (index == 0x01) /* CODE_PAGE */
        {
            vga_set_code_page(word);
            return true;
        }
        return false;
    }
    /* Channels 1 through 14 are this device's own and it acknowledges nothing
     * on them, so the write cannot fail. */
    return true;
}
