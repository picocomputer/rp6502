/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <tusb.h>
#include "sys/com.h"
#include "usb/cdc.h"

static absolute_time_t break_timer;
static bool is_breaking = false;
static uint8_t read_buf[COM_IN_BUF_SIZE];
static bool cdc_port_open = false;

bool cdc_is_open(void)
{
    return cdc_port_open;
}

static void cdc_mark_closed(void)
{
    cdc_port_open = false;
    tud_cdc_write_clear();
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)rts;
    if (dtr)
        cdc_port_open = true;
    else
        cdc_mark_closed();
}

void tud_umount_cb(void)
{
    cdc_mark_closed();
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    cdc_mark_closed();
}

void tud_cdc_tx_complete_cb(uint8_t itf)
{
    (void)itf;
    cdc_port_open = true;
}

static void send_break_ms(uint16_t duration_ms)
{
    break_timer = make_timeout_time_ms(duration_ms);
    is_breaking = true;
    com_set_uart_break(true);
}

void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms)
{
    (void)itf;
    if (duration_ms == 0x0000)
    {
        is_breaking = false;
        com_set_uart_break(false);
    }
    else if (duration_ms == 0xFFFF)
    {
        // Indefinite break — hold until 0x0000 is received
        break_timer = at_the_end_of_time;
        is_breaking = true;
        com_set_uart_break(true);
    }
    else
    {
        send_break_ms(duration_ms);
    }
}

void cdc_task(void)
{
    if (is_breaking && time_reached(break_timer))
    {
        is_breaking = false;
        com_set_uart_break(false);
    }

    // Always drain CDC RX so keyboard input isn't lost when TX FIFO is full
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

    // TX: write com_out to CDC, or discard if no one is listening
    if (tud_cdc_connected() && tud_cdc_write_available())
    {
        if (!com_out_empty())
        {
            while (!com_out_empty() && tud_cdc_write_char(com_out_peek()))
                com_out_read();
            tud_cdc_write_flush();
        }
    }
    else
    {
        while (!com_out_empty())
            com_out_read();
        cdc_mark_closed();
    }
}
