/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sys/cfg.h"
#include "sys/mem.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "sys/rln.h"
#include "sys/vga.h"
#include "ria.pio.h"
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <strings.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_VGA)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// How long to wait for ACK to backchannel enable request
#define VGA_BACKCHANNEL_ACK_MS 2
// How long to wait for version string
#define VGA_VERSION_WATCHDOG_MS 2
// Abandon backchannel after two missed vsync messages (~2/60sec)
#define VGA_VSYNC_WATCHDOG_MS 35

static enum {
    VGA_NOT_FOUND,   // Possibly normal, Pico VGA is optional
    VGA_TESTING,     // Looking for Pico VGA
    VGA_FOUND,       // Found
    VGA_CONNECTED,   // Connected and version string received
    VGA_NO_VERSION,  // Connected but no version string received
    VGA_LOST_SIGNAL, // Definitely an error condition
} vga_state;

static absolute_time_t vga_vsync_timer;
static absolute_time_t vga_version_timer;

#define VGA_VERSION_MESSAGE_SIZE 80
char vga_version_message[VGA_VERSION_MESSAGE_SIZE];
size_t vga_version_message_length;

bool vga_needs_reset;

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

static void vga_backchannel_command(uint8_t byte)
{
    uint8_t scalar = byte & 0xF;
    switch (byte & 0xF0)
    {
    case 0x80:
        vga_vsync_timer = make_timeout_time_ms(VGA_VSYNC_WATCHDOG_MS);
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

static void vga_rln_callback(bool timeout, const char *buf, size_t length)
{
    // VGA1 means VGA on PIX 1
    if (!timeout && length == 4 && !strncasecmp("VGA1", buf, 4))
        vga_state = VGA_FOUND;
    else
        vga_state = VGA_NOT_FOUND;
}

static void vga_connect(void)
{
    // Test if VGA connected
    uint8_t vga_test_buf[4];
    while (stdio_getchar_timeout_us(0) != PICO_ERROR_TIMEOUT)
        tight_loop_contents();
    rln_read_binary(VGA_BACKCHANNEL_ACK_MS, vga_rln_callback, vga_test_buf, sizeof(vga_test_buf));
    vga_pix_backchannel_request();
    vga_state = VGA_TESTING;
    while (vga_state == VGA_TESTING)
        rln_task();
    if (vga_state == VGA_NOT_FOUND)
        return vga_pix_backchannel_disable();

    // Turn on the backchannel
    pio_gpio_init(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_PIN);
    vga_pix_backchannel_enable();

    // Wait for version
    vga_version_message_length = 0;
    vga_version_timer = make_timeout_time_ms(VGA_VERSION_WATCHDOG_MS);
    while (true)
    {
        if (!pio_sm_is_rx_fifo_empty(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM))
        {
            uint8_t byte = pio_sm_get(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM) >> 24;
            if (!(byte & 0x80))
            {
                vga_version_timer = make_timeout_time_ms(VGA_VERSION_WATCHDOG_MS);
                if (byte == '\r' || byte == '\n')
                {
                    if (vga_version_message_length > 0)
                    {
                        vga_vsync_timer = make_timeout_time_ms(VGA_VSYNC_WATCHDOG_MS);
                        vga_state = VGA_CONNECTED;
                        break;
                    }
                }
                else if (vga_version_message_length < VGA_VERSION_MESSAGE_SIZE - 1u)
                {
                    vga_version_message[vga_version_message_length] = byte;
                    vga_version_message[++vga_version_message_length] = 0;
                }
            }
        }
        if (absolute_time_diff_us(get_absolute_time(), vga_version_timer) < 0)
        {
            vga_vsync_timer = make_timeout_time_ms(VGA_VSYNC_WATCHDOG_MS);
            vga_state = VGA_NO_VERSION;
            break;
        }
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
    uint offset = pio_add_program(VGA_BACKCHANNEL_PIO, &vga_backchannel_rx_program);
    pio_sm_config c = vga_backchannel_rx_program_get_default_config(offset);
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

    // Connect and establish backchannel
    vga_connect();
}

void vga_post_reclock(uint32_t sys_clk_khz)
{
    float div = (float)sys_clk_khz * 1000 / (8 * VGA_BACKCHANNEL_BAUDRATE);
    pio_sm_set_clkdiv(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM, div);
}

void vga_task(void)
{
    if (!pio_sm_is_rx_fifo_empty(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM))
    {
        uint8_t byte = pio_sm_get(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM) >> 24;
        if (byte & 0x80)
            vga_backchannel_command(byte);
    }

    if ((vga_state == VGA_CONNECTED || vga_state == VGA_NO_VERSION) &&
        absolute_time_diff_us(get_absolute_time(), vga_vsync_timer) < 0)
    {
        vga_pix_backchannel_disable();
        gpio_set_function(VGA_BACKCHANNEL_PIN, GPIO_FUNC_UART);
        vga_state = VGA_LOST_SIGNAL;
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
        vga_connect();
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

bool vga_connected(void)
{
    return vga_state == VGA_CONNECTED ||
           vga_state == VGA_NO_VERSION;
}

void vga_print_status(void)
{
    const char *msg = "VGA Searching";
    switch (vga_state)
    {
    case VGA_FOUND:
    case VGA_TESTING:
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
