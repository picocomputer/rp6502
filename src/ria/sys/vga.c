/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "str.h"
#include "sys/cfg.h"
#include "sys/com.h"
#include "sys/mem.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "sys/vga.h"
#include "vga.pio.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include <stdio.h>

// How long to wait for ACK to backchannel enable request
#define VGA_BACKCHANNEL_ACK_MS 10
// How long to wait for version string
#define VGA_VERSION_WATCHDOG_MS 2
// Abandon backchannel after two missed vsync messages (~2/60sec)
#define VGA_VSYNC_WATCHDOG_MS 35

static enum {
    VGA_REQUEST_TEST, // Starting state
    VGA_TESTING,      // Looking for Pico VGA
    VGA_VERSIONING,   // Pico VGA PIX device found
    VGA_CONNECTED,    // Connected and version string received
    VGA_NO_VERSION,   // Connected but no version string received
    VGA_NOT_FOUND,    // Possibly normal, Pico VGA is optional
    VGA_LOST_SIGNAL,  // Definitely an error condition
} vga_state;

uint8_t vga_read_buf[4];
static absolute_time_t vga_vsync_watchdog;
static absolute_time_t vga_version_watchdog;

#define VGA_VERSION_MESSAGE_SIZE 80
char vga_version_message[VGA_VERSION_MESSAGE_SIZE];
size_t vga_version_message_length;

bool vga_needs_reset;

static inline void vga_pix_flush()
{
    // It takes four cycles of PHI2 to send a PIX message.
    // Begin by waiting for FIFO to clear.
    while (!pix_fifo_empty())
        tight_loop_contents();
    // Then wait at least six PHI2 cycles for shift registers.
    uint32_t wait = 6000 / cfg_get_phi2_khz();
    busy_wait_us_32(wait ? wait : 1);
}

static inline void vga_pix_backchannel_disable(void)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x04, 0);
}

static inline void vga_pix_backchannel_enable(void)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x04, 1);
}

static inline void vga_pix_backchannel_request(void)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x04, 2);
}

static void vga_read(bool timeout, const char *buf, size_t length)
{
    if (!timeout && length == 4 && !strnicmp("VGA1", buf, 4))
    {
        // IO and buffers need to be in sync before switch
        com_flush();
        // Clear any local echo (UART is seen by PIO)
        while (!pio_sm_is_rx_fifo_empty(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM))
            pio_sm_get(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM);
        // Change direction
        vga_pix_backchannel_enable();
        pio_gpio_init(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_PIN);
        // Wait for version
        vga_state = VGA_VERSIONING;
        vga_version_watchdog = delayed_by_ms(get_absolute_time(), VGA_VERSION_WATCHDOG_MS);
    }
    else
        vga_state = VGA_NOT_FOUND;
}

static void vga_backchannel_command(uint8_t byte)
{
    uint8_t scalar = byte & 0xF;
    switch (byte & 0xF0)
    {
    case 0x80:
        vga_vsync_watchdog = delayed_by_ms(get_absolute_time(), VGA_VSYNC_WATCHDOG_MS);
        static uint8_t vframe;
        if (scalar < (vframe & 0xF))
            vframe = (vframe & 0xF0) + 0x10;
        vframe = (vframe & 0xF0) | scalar;
        REGS(0xFFE3) = vframe;
        ria_trigger_irq();
        break;
    case 0x90:
        pix_ack();
        break;
    case 0xA0:
        pix_nak();
        break;
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

    // Reset Pico VGA
    vga_needs_reset = true;
}

void vga_reclock(uint32_t sys_clk_khz)
{
    float div = (float)sys_clk_khz * 1000 / (8 * VGA_BACKCHANNEL_BAUDRATE);
    pio_sm_set_clkdiv(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM, div);
}

void vga_task(void)
{
    if (vga_state == VGA_REQUEST_TEST)
    {
        vga_state = VGA_TESTING;
        com_read_binary(VGA_BACKCHANNEL_ACK_MS, vga_read, vga_read_buf, 4);
        vga_pix_backchannel_request();
    }

    if (!pio_sm_is_rx_fifo_empty(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM))
    {
        uint8_t byte = pio_sm_get(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM) >> 24;

        if (byte & 0x80)
            vga_backchannel_command(byte);
        else if (vga_state == VGA_VERSIONING)
        {
            vga_version_watchdog = delayed_by_ms(get_absolute_time(), VGA_VERSION_WATCHDOG_MS);
            if (byte == '\r' || byte == '\n')
            {
                if (vga_version_message_length > 0 && vga_state == VGA_VERSIONING)
                {
                    vga_vsync_watchdog = delayed_by_ms(get_absolute_time(), VGA_VSYNC_WATCHDOG_MS);
                    vga_state = VGA_CONNECTED;
                    vga_version_message_length = 0;
                    vga_print_status();
                }
            }
            else if (vga_version_message_length < VGA_VERSION_MESSAGE_SIZE - 1u)
            {
                vga_version_message[vga_version_message_length++] = byte;
                vga_version_message[vga_version_message_length] = 0;
            }
        }
    }

    if (vga_state == VGA_VERSIONING &&
        absolute_time_diff_us(get_absolute_time(), vga_version_watchdog) < 0)
    {
        vga_vsync_watchdog = delayed_by_ms(get_absolute_time(), VGA_VSYNC_WATCHDOG_MS);
        vga_state = VGA_NO_VERSION;
        vga_print_status();
    }

    if ((vga_state == VGA_CONNECTED || vga_state == VGA_NO_VERSION) &&
        absolute_time_diff_us(get_absolute_time(), vga_vsync_watchdog) < 0)
    {
        gpio_set_function(VGA_BACKCHANNEL_PIN, GPIO_FUNC_UART);
        vga_pix_backchannel_disable();
        vga_pix_flush();
        vga_state = VGA_LOST_SIGNAL;
        // This is usually futile, but print an error message anyway
        printf("?");
        vga_print_status();
    }

    if (vga_needs_reset)
    {
        vga_needs_reset = false;
        pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x00, cfg_get_vga());
    }
}

void vga_run(void)
{
    // It's normal to lose signal during Pico VGA development.
    // Attempt to restart when a 6502 program is run.
    if (vga_state == VGA_LOST_SIGNAL && !ria_active())
        vga_state = VGA_REQUEST_TEST;
}

void vga_stop(void)
{
    // We want to reset only when program stops,
    // otherwise video flickers after every ria job.
    if (!ria_active())
        vga_needs_reset = true;
}

void vga_reset(void)
{
    vga_needs_reset = true;
}

bool vga_set_vga(uint32_t display_type)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x00, display_type);
    return true;
}

bool vga_active(void)
{
    return vga_state == VGA_REQUEST_TEST ||
           vga_state == VGA_TESTING ||
           vga_state == VGA_VERSIONING;
}

bool vga_backchannel(void)
{
    return vga_state == VGA_VERSIONING ||
           vga_state == VGA_CONNECTED ||
           vga_state == VGA_NO_VERSION;
}

void vga_print_status(void)
{
    const char *msg = "VGA Searching";
    switch (vga_state)
    {
    case VGA_REQUEST_TEST:
    case VGA_TESTING:
    case VGA_VERSIONING:
        break;
    case VGA_CONNECTED:
        msg = vga_version_message;
        break;
    case VGA_NO_VERSION:
        msg = "VGA Version Unknown";
        break;
    case VGA_NOT_FOUND:
        msg = "VGA Not Found";
        break;
    case VGA_LOST_SIGNAL:
        msg = "VGA Signal Lost";
        break;
    }
    puts(msg);
}
