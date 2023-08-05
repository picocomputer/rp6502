/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "str.h"
#include "com.h"
#include "pix.h"
#include "vga.h"
#include "pico/stdlib.h"

#define VGA_PIX_SETUP_CHAN 0x0
#define VGA_PIX_CONTROL_CHAN 0xF

#define VGA_BACKCHANNEL_PIN COM_UART_TX_PIN

// How long to wait for ACK to backchannel enable request
#define VGA_TEST_MS 1

static enum {
    VGA_REQUEST_TEST,
    VGA_TESTING,
    VGA_IDLE,
} vga_state;

static bool vga_backchannel_activated;

uint8_t vga_read_buf[4];

static void vga_read(bool timeout, size_t length)
{
    vga_state = VGA_IDLE;
    if (!timeout && length == 4 && !strnicmp("VGA1", (char *)vga_read_buf, 4))
    {
        // printf("BACKCHANNEL!\n");
        com_flush();

        // TODO finish UART Rx PIO
        //  pio_gpio_init(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_PIN);
        gpio_init(VGA_BACKCHANNEL_PIN);
        gpio_set_dir(VGA_BACKCHANNEL_PIN, false);

        pix_send_blocking(PIX_VGA_DEV, VGA_PIX_CONTROL_CHAN, 0x04, 1);
        vga_backchannel_activated = true;
    }
}

void vga_init()
{
    // Disable backchannel for the case where RIA reboots and VGA doesn't
    pix_send_blocking(PIX_VGA_DEV, VGA_PIX_CONTROL_CHAN, 0x04, 0);
}

void vga_task()
{
    if (vga_state == VGA_REQUEST_TEST)
    {
        vga_state = VGA_TESTING;
        com_read_binary(vga_read_buf, 4, VGA_TEST_MS, vga_read);
        pix_send_blocking(PIX_VGA_DEV, VGA_PIX_CONTROL_CHAN, 0x04, 2);
    }
}

bool vga_active()
{
    return vga_state != VGA_IDLE;
}

bool vga_backchannel()
{
    return vga_backchannel_activated;
}
