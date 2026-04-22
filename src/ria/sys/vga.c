/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon/mon.h"
#include "str/rln.h"
#include "str/str.h"
#include "sys/com.h"
#include "sys/cfg.h"
#include "sys/mem.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "sys/vga.h"
#include "ria.pio.h"
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <strings.h>
#include <stdio.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_VGA)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// How long to wait for ACK to backchannel enable request
#define VGA_BACKCHANNEL_ACK_MS 2
// How long to wait before aborting version string
#define VGA_VERSION_WATCHDOG_MS 2
// Abandon backchannel after two missed vsync messages
// with 10ms added for UART drain and safety margin
// when changing canvas or timing.
#define VGA_VSYNC_WATCHDOG_MS 44

static enum {
    VGA_NOT_FOUND,       // Possibly normal, RP6502-VGA is optional
    VGA_TESTING,         // Looking for RP6502-VGA
    VGA_FOUND,           // Found
    VGA_CONNECTED,       // Connected and version string received
    VGA_NO_VERSION,      // Connected but no version string received
    VGA_CONNECTION_LOST, // Definitely an error condition
} vga_state;

static bool vga_needs_reset = true;
static uint8_t vga_display_type;
static volatile uint32_t vga_vsync_deadline;
static absolute_time_t vga_version_timer;
static uint8_t vga_vsync_frame;

#define VGA_VERSION_MESSAGE_SIZE 64
static char vga_version_message[VGA_VERSION_MESSAGE_SIZE];
static size_t vga_version_message_length;

static inline void vga_pix_backchannel_disable(void)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x04, 0);
}

void vga_set_tel_console_active(bool active)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x02, active ? 1 : 0);
}

void vga_set_code_page(uint16_t cp)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x01, cp);
}

static inline void vga_pix_backchannel_enable(void)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x04, 1);
}

static inline void vga_pix_backchannel_request(void)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x04, 2);
}

static inline void vga_backchannel_irq_enable(void)
{
    pio_set_irq0_source_enabled(VGA_BACKCHANNEL_PIO,
                                pis_sm2_rx_fifo_not_empty, true);
}

static inline void vga_backchannel_irq_disable(void)
{
    pio_set_irq0_source_enabled(VGA_BACKCHANNEL_PIO,
                                pis_sm2_rx_fifo_not_empty, false);
}

static inline void vga_backchannel_command(uint8_t byte)
{
    switch (byte & 0xF0)
    {
    case 0x80:
    {
        uint8_t frame = byte & 0xF;
        vga_vsync_deadline = time_us_32() + VGA_VSYNC_WATCHDOG_MS * 1000;
        if (frame < (vga_vsync_frame & 0xF))
            vga_vsync_frame += 0x10;
        vga_vsync_frame = (vga_vsync_frame & 0xF0) | frame;
        REGS(0xFFE3) = vga_vsync_frame;
        ria_trigger_irq();
        break;
    }
    case 0x90:
        pix_ack();
        break;
    case 0xA0:
        pix_nak();
        break;
    }
}

static void
    __attribute__((optimize("O3")))
    __isr
    __time_critical_func(vga_backchannel_irq_handler)(void)
{
    while (!pio_sm_is_rx_fifo_empty(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM))
    {
        uint8_t byte = pio_sm_get(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM) >> 24;
        if (byte & 0x80)
            vga_backchannel_command(byte);
    }
}

static void vga_rln_callback(bool timeout)
{
    // VGA1 means VGA on PIX 1
    if (!timeout && mbuf_len == 4 && !strncasecmp(STR_VGA1, (char *)mbuf, 4))
        vga_state = VGA_FOUND;
    else
        vga_state = VGA_NOT_FOUND;
}

static void vga_connect(void)
{
    vga_backchannel_irq_disable();
    while (stdio_getchar_timeout_us(0) != PICO_ERROR_TIMEOUT)
        tight_loop_contents();

    // Test if VGA connected
    mem_read_mbuf(VGA_BACKCHANNEL_ACK_MS, vga_rln_callback, 4);
    vga_pix_backchannel_request();
    vga_state = VGA_TESTING;
    while (vga_state == VGA_TESTING)
        mem_task();
    if (vga_state == VGA_NOT_FOUND)
    {
        vga_pix_backchannel_disable();
        return;
    }

    // Turn on the backchannel
    pio_gpio_init(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_PIN);
    // Drain bytes the PIO sampled off the pin while it was driven by UART TX
    while (!pio_sm_is_rx_fifo_empty(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM))
        (void)pio_sm_get(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM);
    vga_pix_backchannel_enable();

    // Wait for version
    vga_version_message_length = 0;
    vga_version_timer = make_timeout_time_ms(VGA_VERSION_WATCHDOG_MS);
    while (true)
    {
        if (!pio_sm_is_rx_fifo_empty(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM))
        {
            uint8_t byte = pio_sm_get(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM) >> 24;
            if (byte & 0x80)
            {
                vga_backchannel_command(byte);
            }
            else
            {
                vga_version_timer = make_timeout_time_ms(VGA_VERSION_WATCHDOG_MS);
                if (byte == '\r' || byte == '\n')
                {
                    if (vga_version_message_length > 0)
                    {
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
        if (time_reached(vga_version_timer))
        {
            vga_state = VGA_NO_VERSION;
            break;
        }
    }

    // Enable backchannel IRQ now that version string phase is complete
    vga_backchannel_irq_enable();
    vga_vsync_deadline = time_us_32() + VGA_VSYNC_WATCHDOG_MS * 1000;
}

void vga_init(void)
{
    // Disable backchannel for the case where RIA reboots and VGA doesn't
    vga_pix_backchannel_disable();
    vga_set_tel_console_active(false);

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

    // Set up PIO IRQ for backchannel RX, source stays disabled until connect
    irq_set_exclusive_handler(PIO1_IRQ_0, vga_backchannel_irq_handler);
    irq_set_priority(PIO1_IRQ_0, PICO_DEFAULT_IRQ_PRIORITY - 0x10);
    irq_set_enabled(PIO1_IRQ_0, true);

    // Re-send in case the first disable was lost
    vga_pix_backchannel_disable();

    // Connect and establish backchannel
    vga_connect();
}

void vga_task(void)
{
    if ((vga_state == VGA_CONNECTED || vga_state == VGA_NO_VERSION) &&
        (int32_t)(time_us_32() - vga_vsync_deadline) >= 0)
    {
        vga_backchannel_irq_disable();
        vga_pix_backchannel_disable();
        gpio_set_function(VGA_BACKCHANNEL_PIN, GPIO_FUNC_UART);
        vga_state = VGA_CONNECTION_LOST;
        mon_add_response_str(STR_ERR_VGA_CONNECTION_LOST);
    }

    if (vga_needs_reset)
    {
        vga_needs_reset = false;
        pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x00, vga_display_type);
    }
}

void vga_run(void)
{
    // It's normal to lose signal during Pico VGA development.
    // Attempt to restart when a 6502 program is run.
    if (vga_state == VGA_CONNECTION_LOST && !ria_active())
        vga_connect();
}

void vga_stop(void)
{
    // We want to reset only when program stops,
    // otherwise video flickers after every ria job.
    if (!ria_active())
        vga_needs_reset = true;
}

void vga_break(void)
{
    vga_needs_reset = true;
}

bool vga_connected(void)
{
    return vga_state == VGA_CONNECTED ||
           vga_state == VGA_NO_VERSION;
}

int vga_boot_response(char *buf, size_t buf_size, int state)
{
    if (!vga_connected())
        return -1;
    return vga_status_response(buf, buf_size, state);
}

int vga_status_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    const char *msg = STR_VGA_SEARCHING;
    switch (vga_state)
    {
    case VGA_TESTING:
    case VGA_FOUND:
        break;
    case VGA_CONNECTED:
        msg = vga_version_message;
        break;
    case VGA_NO_VERSION:
        msg = STR_VGA_VERSION_UNKNOWN;
        break;
    case VGA_NOT_FOUND:
        msg = STR_VGA_NOT_FOUND;
        break;
    case VGA_CONNECTION_LOST:
        msg = STR_VGA_CONNECTION_LOST;
        break;
    }
    snprintf(buf, buf_size, "%s\n", msg);
    return -1;
}

void vga_load_display_type(const char *str)
{
    str_parse_uint8(&str, &vga_display_type);
    if (vga_display_type > 2)
        vga_display_type = 0;
}

bool vga_set_display_type(uint8_t display_type)
{
    if (display_type > 2)
        return false;
    if (vga_display_type != display_type)
    {
        vga_display_type = display_type;
        vga_needs_reset = true;
        cfg_save();
    }
    return true;
}

uint8_t vga_get_display_type(void)
{
    return vga_display_type;
}

const char *vga_get_display_type_verbose(void)
{
    const char *const labels[] = {STR_VGA_DISPLAY_TYPE_0,
                                  STR_VGA_DISPLAY_TYPE_1,
                                  STR_VGA_DISPLAY_TYPE_2};
    return labels[vga_display_type];
}
