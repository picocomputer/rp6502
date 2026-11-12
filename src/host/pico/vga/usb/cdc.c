/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <tusb.h>
#include "vga/com/manifold.h"
#include "vga/usb/cdc.h"

static absolute_time_t break_timer;
static bool is_breaking = false;
static uint8_t read_buf[MANIFOLD_IN_BUF_SIZE];

void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms)
{
    (void)itf;
    if (duration_ms == 0)
    {
        is_breaking = false;
        manifold_set_uart_break(false);
        return;
    }
    // 0xFFFF means hold indefinitely until a 0 is received.
    break_timer = (duration_ms == 0xFFFF)
                      ? at_the_end_of_time
                      : make_timeout_time_ms(duration_ms);
    is_breaking = true;
    manifold_set_uart_break(true);
}

void cdc_task(void)
{
    if (is_breaking && time_reached(break_timer))
    {
        is_breaking = false;
        manifold_set_uart_break(false);
    }

    if (tud_cdc_available())
    {
        size_t bufsize = manifold_in_free();
        if (bufsize > 0)
        {
            size_t data_len = tud_cdc_read(read_buf, bufsize);
            for (size_t i = 0; i < data_len; i++)
                manifold_in_write(read_buf[i]);
        }
    }

    // OUT is purged while the device is unmounted or suspended, while DTR is
    // clear, and when both the CDC TX FIFO and OUT are full, because the
    // manifold's stdio driver calls cdc_task in a loop until OUT has room.
    if (!tud_cdc_connected() ||
        (!tud_cdc_write_available() && manifold_out_full()))
    {
        while (!manifold_out_empty())
            manifold_out_read();
        tud_cdc_write_clear();
        return;
    }
    if (tud_cdc_write_available())
    {
        while (!manifold_out_empty() && tud_cdc_write_char(manifold_out_peek()))
            manifold_out_read();
        tud_cdc_write_flush();
    }
}
