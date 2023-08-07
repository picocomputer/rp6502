/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "str.h"
#include "sys/com.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "sys/vga.h"
#include "vga.pio.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include <stdio.h>

#define VGA_BACKCHANNEL_PIN COM_UART_TX_PIN
#define VGA_BACKCHANNEL_BAUDRATE 115200
#define VGA_BACKCHANNEL_PIO pio0
#define VGA_BACKCHANNEL_SM 2

// How long to wait for ACK to backchannel enable request
#define VGA_TEST_MS 1

// Abandon backchannel after two missed vsync messages (~2/60sec)
#define VGA_WATCHDOG_MS 35

static enum {
    VGA_REQUEST_TEST, // Starting state
    VGA_TESTING,      // Looking for Pico VGA
    VGA_CONNECTED,    // Pico VGA PIX device found and working
    VGA_NOT_FOUND,    // Possibly normal, Pico VGA is optional
    VGA_LOST_SIGNAL,  // Definitely an error condition
} vga_state;

uint8_t vga_read_buf[4];
static absolute_time_t vga_watchdog;

static inline void vga_pix_backchannel_disable(void)
{
    pix_send_blocking(PIX_VGA_DEV, 0xF, 0x04, 0);
}

static inline void vga_pix_backchannel_enable(void)
{
    pix_send_blocking(PIX_VGA_DEV, 0xF, 0x04, 1);
}

static inline void vga_pix_backchannel_request(void)
{
    pix_send_blocking(PIX_VGA_DEV, 0xF, 0x04, 2);
}

static void vga_read(bool timeout, size_t length)
{
    if (!timeout && length == 4 && !strnicmp("VGA1", (char *)vga_read_buf, 4))
    {
        printf("VGA PIX device connected\n");
        vga_state = VGA_CONNECTED;
        com_flush();
        pio_gpio_init(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_PIN);
        vga_pix_backchannel_enable();
    }
    else
    {
        vga_state = VGA_NOT_FOUND;
    }
}

void vga_init(void)
{
    // Disable backchannel for the case where RIA reboots and VGA doesn't
    vga_pix_backchannel_disable();

    // Program a UART Rx in PIO
    pio_sm_set_consecutive_pindirs(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM,
                                   VGA_BACKCHANNEL_PIN, 1, false);
    gpio_pull_up(VGA_BACKCHANNEL_PIN);
    uint offset = pio_add_program(VGA_BACKCHANNEL_PIO, &uart_rx_mini_program);
    pio_sm_config c = uart_rx_mini_program_get_default_config(offset);
    sm_config_set_in_pins(&c, VGA_BACKCHANNEL_PIN); // for WAIT, IN
    sm_config_set_in_shift(&c, true, true, 8);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    float div = (float)clock_get_hz(clk_sys) / (8 * VGA_BACKCHANNEL_BAUDRATE);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM, offset, &c);
    pio_sm_set_enabled(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM, true);

    // Disable backchannel again, for safety.
    vga_pix_backchannel_disable();
}

void vga_reclock(void)
{
    float div = (float)clock_get_hz(clk_sys) / (8 * VGA_BACKCHANNEL_BAUDRATE);
    pio_sm_set_clkdiv(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM, div);
}

void vga_task(void)
{
    if (vga_state == VGA_REQUEST_TEST)
    {
        vga_state = VGA_TESTING;
        com_read_binary(vga_read_buf, 4, VGA_TEST_MS, vga_read);
        vga_pix_backchannel_request();
    }

    if (!pio_sm_is_rx_fifo_empty(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM))
    {
        vga_watchdog = delayed_by_ms(get_absolute_time(), VGA_WATCHDOG_MS);
        // TODO watchdog
        uint8_t byte = pio_sm_get(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM) >> 24;
        if (byte & 0x80)
        {
            static uint8_t vframe;
            uint8_t this_frame = byte & 0x7F;
            if (this_frame < (vframe & 0x7F))
                vframe ^= 0x80;
            vframe = (vframe & 0x80) | this_frame;
            REGS(0xFFE3) = vframe;
            // printf("{%d}", vframe);
        }
        else
        {
            // TODO incoming version string
            // printf("[%c]", byte);
        }
    }

    if (vga_state == VGA_CONNECTED &&
        absolute_time_diff_us(get_absolute_time(), vga_watchdog) < 0)
    {
        printf("VGA PIX backchannel lost signal\n");
        vga_state = VGA_LOST_SIGNAL;
        gpio_set_function(VGA_BACKCHANNEL_PIN, GPIO_FUNC_UART);
    }
}

bool vga_active(void)
{
    return vga_state == VGA_REQUEST_TEST || vga_state == VGA_TESTING;
}

bool vga_backchannel(void)
{
    return vga_state == VGA_CONNECTED;
}

void vga_print_status(void)
{
    const char *msg = "Looking for PIX VGA device";
    switch (vga_state)
    {
    case VGA_REQUEST_TEST:
        break;
    case VGA_TESTING:
        break;
    case VGA_CONNECTED:
        msg = "TODO";
        break;
    case VGA_NOT_FOUND:
        msg = "Not found";
        break;
    case VGA_LOST_SIGNAL:
        msg = "Signal lost";
        break;
    }
    puts(msg);
}
