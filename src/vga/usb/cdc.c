/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <tusb.h>
#include "sys/com.h"
#include "usb/cdc.h"

static absolute_time_t break_timer;
static bool is_breaking = false;
static uint8_t read_buf[COM_IN_BUF_SIZE];
static bool cdc_ready = false;

bool cdc_is_ready(void)
{
    return cdc_ready;
}

static void cdc_mark_not_ready(void)
{
    cdc_ready = false;
    while (!com_out_empty())
        com_out_read();
    tud_cdc_write_clear();
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)rts;
    if (dtr)
        cdc_ready = true;
    else
        cdc_mark_not_ready();
}

void tud_umount_cb(void)
{
    cdc_mark_not_ready();
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    cdc_mark_not_ready();
}

// Successful TX is evidence the host is reading; treat as ready even if
// DTR never asserted. cdc_is_ready() callers rely on this to avoid
// duplicating ANSI replies that the host terminal will generate itself.
void tud_cdc_tx_complete_cb(uint8_t itf)
{
    (void)itf;
    cdc_ready = true;
}

void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms)
{
    (void)itf;
    if (duration_ms == 0)
    {
        is_breaking = false;
        com_set_uart_break(false);
        return;
    }
    // 0xFFFF means hold indefinitely until a 0 is received.
    break_timer = (duration_ms == 0xFFFF)
                      ? at_the_end_of_time
                      : make_timeout_time_ms(duration_ms);
    is_breaking = true;
    com_set_uart_break(true);
}

void cdc_task(void)
{
    if (is_breaking && time_reached(break_timer))
    {
        is_breaking = false;
        com_set_uart_break(false);
    }

    // Drain USB RX independently of TX so host input isn't stalled
    // by a blocked UART TX path.
    if (tud_cdc_available())
    {
        size_t bufsize = com_in_free();
        if (bufsize > 0)
        {
            size_t data_len = tud_cdc_read(read_buf, bufsize);
            for (size_t i = 0; i < data_len; i++)
                com_in_write(read_buf[i]);
        }
    }

    // TX stall recovery: disconnected, or both FIFOs full with no forward
    // progress. Mark not ready (which purges com_out) to unblock com_out_chars().
    if (!tud_cdc_connected() ||
        (!tud_cdc_write_available() && com_out_full()))
    {
        cdc_mark_not_ready();
        return;
    }
    if (tud_cdc_write_available())
    {
        while (!com_out_empty() && tud_cdc_write_char(com_out_peek()))
            com_out_read();
        tud_cdc_write_flush();
    }
}
