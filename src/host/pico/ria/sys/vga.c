/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/mon/mon.h"
#include "core/com/rln.h"
#include "core/str/str.h"
#include "core/com/com.h"
#include "ria/com/manifold.h"
#include "ria/sys/cfg.h"
#include "core/ria/regs.h"
#include "ria/sys/mbuf.h"
#include "ria/sys/pix.h"
#include "ria/sys/ria.h"
#include "ria/sys/rp2350.h"
#include "ria/sys/vga.h"
#include "core/sys/config.h"
#include "core/str/oem.h"
#include "core/term/font.h"
#include "core/vga/mode/mode0.h"
#include "ria/term/cells.h"
#include "ria.pio.h"
#include "machine.h"
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/timer.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

// How long to wait for ACK to backchannel enable request
#define VGA_BACKCHANNEL_ACK_MS 3
// How long to wait before aborting version string
#define VGA_VERSION_WATCHDOG_MS 3
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
static vga_canvas_t vga_canvas_current = vga_canvas_console;
static volatile uint32_t vga_vsync_deadline;
static absolute_time_t vga_version_timer;
static uint8_t vga_vsync_frame;

static int vga_vsync_alarm = -1;
static bool vga_vsync_alarm_arms;
static volatile bool vga_vsync_trap_armed;
static volatile bool vga_vsync_trap_fired;

#define VGA_VERSION_MESSAGE_SIZE 64
static char vga_version_message[VGA_VERSION_MESSAGE_SIZE];
static size_t vga_version_message_length;

static inline void vga_pix_backchannel_disable(void)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x04, 0);
}

/* The console's terminal follows the VGA's (ria/term/cells.c). Every change
 * the VGA makes to its terminal is made here at the moment the RIA sends what
 * causes it. The console's bytes go to the VGA through the same PIX queue,
 * so the two terminals take them all in the same order. */

/* The code page the VGA's fonts hold, or VGA_CODE_PAGE_UNKNOWN. Choosing a
 * different one resets its terminal. */
#define VGA_CODE_PAGE_UNKNOWN 0xFFFF
static uint16_t vga_code_page_sent = VGA_CODE_PAGE_UNKNOWN;

/* The display type the VGA was last sent, which sizes its console. */
static uint8_t vga_display_sent;

/* The canvas and mode registers of channel 0 as the VGA holds them. */
static uint16_t vga_xregs[8];

void vga_set_code_page(uint16_t cp)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x01, cp);
    if (!vga_connected())
        return;
    cp = font_code_page_of(cp);
    if (cp != vga_code_page_sent)
        cells_RIS();
    vga_code_page_sent = cp;
}

/* The VGA's console terminal on a display: 32 rows on SXGA (2), otherwise 30. */
static uint8_t vga_console_rows(uint8_t display)
{
    uint16_t xregs[8] = {0};
    int16_t begin, end;
    uint8_t cols, rows = 0;
    mode0_place(640, display == 2 ? 512 : 480, xregs, &begin, &end, &cols, &rows);
    return rows;
}

/* DISPLAY: the VGA goes back to the console canvas, which sizes the console
 * for the old display, sizes it for the new one, and does the system's soft
 * reset. */
static void vga_follow_display(uint8_t display)
{
    if (vga_canvas_current != vga_canvas_console)
        cells_set_height(80, vga_console_rows(vga_display_sent));
    cells_set_height(80, vga_console_rows(display));
    cells_RIS_no_clear();
    vga_display_sent = display;
}

/* With no VGA the console is a terminal on the UART, taken to be 80x24,
 * and that terminal answers the queries, so the console's terminal is quiet
 * by default. */
static void vga_follow_none(void)
{
    cells_RIS();
    cells_set_height(80, 24);
    com_set_reply_default(true);
}
void vga_load_code_page(uint16_t cp)
{
    vga_set_code_page(cp);
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
    pio_set_irq1_source_enabled(VGA_BACKCHANNEL_PIO,
                                pis_sm3_rx_fifo_not_empty, true);
}

static inline void vga_backchannel_irq_disable(void)
{
    pio_set_irq1_source_enabled(VGA_BACKCHANNEL_PIO,
                                pis_sm3_rx_fifo_not_empty, false);
}

static void HOST_IN_RAM_FUNC vga_vsync_trap_disarm(void)
{
    if (!vga_vsync_trap_armed)
        return;
    vga_vsync_trap_armed = false;
    pio_set_irq0_source_enabled(VGA_BACKCHANNEL_PIO, pis_interrupt0, false);
    // A trap interrupt that is already pending would otherwise run the handler
    // after the disarm and raise an extra VSYNC.
    irq_clear(VGA_VSYNC_TRAP_IRQ);
}

/* The alarm is driven through TIMER0's registers rather than the SDK's alarm
 * callbacks, so everything its interrupt runs is in RAM. Returns true when
 * us has already passed, with the alarm disarmed. */
static bool HOST_IN_RAM_FUNC vga_vsync_alarm_at(uint32_t us)
{
    timer_hw->alarm[vga_vsync_alarm] = us;
    if ((int32_t)(us - timer_hw->timerawl) > 0)
        return false;
    timer_hw->armed = 1u << vga_vsync_alarm;
    return true;
}

static void HOST_ISR_FUNC vga_vsync_alarm_irq_handler(void)
{
    timer_hw->intr = 1u << vga_vsync_alarm;
    if (!vga_vsync_alarm_arms)
    {
        vga_vsync_trap_disarm();
        return;
    }
    // The trap is armed only after the version string phase ends, because the
    // start bit of a version byte would trigger it.
    if (vga_state != VGA_CONNECTED && vga_state != VGA_NO_VERSION)
        return;
    vga_vsync_trap_fired = false;
    vga_vsync_trap_armed = true;
    vga_vsync_alarm_arms = false;
    pio_interrupt_clear(VGA_BACKCHANNEL_PIO, 0);
    pio_set_irq0_source_enabled(VGA_BACKCHANNEL_PIO, pis_interrupt0, true);
    if (vga_vsync_alarm_at(time_us_32() + 2 * VGA_VSYNC_WINDOW_US))
        vga_vsync_trap_disarm();
}

static void HOST_IN_RAM_FUNC vga_vsync_reschedule(void)
{
    if (vga_vsync_alarm < 0)
        return;
    vga_vsync_trap_disarm();
    vga_vsync_alarm_arms = true;
    vga_vsync_alarm_at(time_us_32() - VGA_VSYNC_TRANSIT_US +
                       VGA_VSYNC_PERIOD_US - VGA_VSYNC_WINDOW_US);
}

static void HOST_ISR_FUNC vga_vsync_trap_irq_handler(void)
{
    pio_set_irq0_source_enabled(VGA_BACKCHANNEL_PIO, pis_interrupt0, false);
    vga_vsync_trap_armed = false;
    vga_vsync_trap_fired = true;
    // The frame number written here is a guess. The backchannel byte for this
    // VSYNC arrives about 74 us later, and when its frame number differs,
    // vga_backchannel_command writes the corrected number and calls
    // ria_trigger_vsync again.
    REGS(0xFFE3) = ++vga_vsync_frame;
    ria_trigger_vsync();
}

static __force_inline void vga_backchannel_command(uint8_t byte)
{
    switch (byte & 0xF0)
    {
    case 0x80:
    {
        uint8_t frame = byte & 0xF;
        vga_vsync_deadline = time_us_32() + VGA_VSYNC_WATCHDOG_MS * 1000;
        vga_vsync_reschedule();
        if (vga_vsync_trap_fired)
        {
            vga_vsync_trap_fired = false;
            if ((vga_vsync_frame & 0xF) == frame)
                break;
        }
        if (frame < (vga_vsync_frame & 0xF))
            vga_vsync_frame += 0x10;
        vga_vsync_frame = (vga_vsync_frame & 0xF0) | frame;
        REGS(0xFFE3) = vga_vsync_frame;
        ria_trigger_vsync();
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

static void HOST_ISR_FUNC vga_backchannel_irq_handler(void)
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

/* Only the UART, the VGA's own link, is cleared and read, so a reconnect at
 * the run after a lost link keeps the keyboard and telnet type-ahead. */
static void HOST_IN_FLASH_FUNC vga_connect(void)
{
    vga_backchannel_irq_disable();
    com_rx_clear_source(COM_SOURCE_UART);

    // Test if VGA connected
    mbuf_read(COM_SOURCE_UART, VGA_BACKCHANNEL_ACK_MS, vga_rln_callback, 4);
    vga_pix_backchannel_request();
    vga_state = VGA_TESTING;
    while (vga_state == VGA_TESTING)
    {
        manifold_uart_drain_rx();
        mbuf_task();
    }
    if (vga_state == VGA_NOT_FOUND)
    {
        vga_pix_backchannel_disable();
        gpio_set_function(VGA_BACKCHANNEL_PIN, GPIO_FUNC_UART);
        vga_follow_none();
        return;
    }

    // Turn on the backchannel
    pio_gpio_init(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_PIN);
    // Drain bytes the PIO sampled off the pin before the VGA took it
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

    /* The VGA may have kept its state while the RIA restarted, or started
     * fresh while the RIA ran. DISPLAY and the code page are sent again, and
     * a hard reset follows them to both terminals, so the two start alike. */
    vga_needs_reset = true;
    vga_code_page_sent = VGA_CODE_PAGE_UNKNOWN;
    if (oem_get_code_page_run()) /* at boot oem_init sends it */
        vga_set_code_page(oem_get_code_page_run());
    manifold_write_to(COM_TERM_SCREEN, STR_TERM_HARD_RESET, STR_TERM_HARD_RESET_LEN);
    com_set_reply_default(false);
    /* A VGA of the firmware before the RIA answered the queries still has
     * its own answers on; this turns them off, so an update of either chip
     * first works. A VGA since then ignores it. */
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x02, 1);
}

void HOST_IN_FLASH_FUNC vga_init(void)
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
    float div = (float)clock_get_hz(clk_sys) / (16 * VGA_BACKCHANNEL_BAUDRATE);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM, offset, &c);
    pio_sm_set_enabled(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM, true);

    // Set up PIO IRQ for backchannel RX, source stays disabled until connect
    irq_set_exclusive_handler(VGA_BACKCHANNEL_IRQ, vga_backchannel_irq_handler);
    irq_set_priority(VGA_BACKCHANNEL_IRQ, RP2350_IRQ_PRIORITY_VGA);
    irq_set_enabled(VGA_BACKCHANNEL_IRQ, true);

    // The trap's source stays disabled until an alarm arms it
    irq_set_exclusive_handler(VGA_VSYNC_TRAP_IRQ, vga_vsync_trap_irq_handler);
    irq_set_priority(VGA_VSYNC_TRAP_IRQ, RP2350_IRQ_PRIORITY_VGA);
    irq_set_enabled(VGA_VSYNC_TRAP_IRQ, true);
    vga_vsync_alarm = (int)hardware_alarm_claim_unused(true);
    const uint alarm_irq = hardware_alarm_get_irq_num((uint)vga_vsync_alarm);
    irq_set_exclusive_handler(alarm_irq, vga_vsync_alarm_irq_handler);
    // The alarm has the same priority as the backchannel and trap interrupts,
    // so none of the three handlers can preempt another in the middle of arming
    // or disarming the trap.
    irq_set_priority(alarm_irq, RP2350_IRQ_PRIORITY_VGA);
    hw_set_bits(&timer_hw->inte, 1u << vga_vsync_alarm);
    irq_set_enabled(alarm_irq, true);

    // Re-send in case the first disable was lost
    vga_pix_backchannel_disable();

    // Connect and establish backchannel
    vga_connect();
}

void HOST_IN_RAM_FUNC vga_task(void)
{
    if ((vga_state == VGA_CONNECTED || vga_state == VGA_NO_VERSION) &&
        (int32_t)(time_us_32() - vga_vsync_deadline) >= 0)
    {
        vga_backchannel_irq_disable();
        vga_vsync_alarm_arms = false;
        if (vga_vsync_alarm >= 0)
            timer_hw->armed = 1u << vga_vsync_alarm;
        vga_vsync_trap_disarm();
        vga_pix_backchannel_disable();
        gpio_set_function(VGA_BACKCHANNEL_PIN, GPIO_FUNC_UART);
        vga_state = VGA_CONNECTION_LOST;
        mon_add_response_utf8(S(STR_ERR_VGA_CONNECTION_LOST));
        vga_follow_none();
    }

    if (vga_needs_reset)
    {
        vga_needs_reset = false;
        // DISPLAY resets the terminal, so everything printed before the stop
        // goes out first: an escape sequence cut in half by the reset would
        // print the rest of itself as text.
        manifold_flush();
        // VGA-side pix_ch15_xreg DISPLAY case calls vga_xreg_canvas(NULL),
        // which resets canvas to vga_canvas_console. Mirror that here.
        uint8_t display = vga_get_display_type();
        pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x00, display);
        if (vga_connected())
            vga_follow_display(display);
        vga_canvas_current = vga_canvas_console;
        memset(vga_xregs, 0, sizeof vga_xregs);
    }
}

void vga_run(void)
{
    // It's normal to lose signal during Pico VGA development.
    // Attempt to restart when a 6502 program is run.
    if (vga_state == VGA_CONNECTION_LOST)
        vga_connect();
}

void vga_stop(void)
{
    vga_needs_reset = true;
}

void vga_break(void)
{
    vga_needs_reset = true;
}

vga_canvas_t vga_get_canvas(void)
{
    return vga_canvas_current;
}

void HOST_IN_RAM_FUNC vga_canvas_size(int *w, int *h)
{
    vga_canvas_geometry(vga_canvas_current, w, h);
}

// The VGA draws the console on a 1280x1024 display as a 640x512 view, and on
// every other display as 640x480.
void HOST_IN_RAM_FUNC vga_console_size(int *w, int *h)
{
    *w = 640;
    *h = vga_get_display_type() == 2 ? 512 : 480;
}

/* The VGA's timing for the display type and canvas: SXGA for every canvas on a
 * 5:4 display, 720p for a 16:9 canvas on an HD display, and VGA otherwise. */
void HOST_IN_RAM_FUNC vga_display_size(int *w, int *h)
{
    uint8_t type = vga_get_display_type();
    if (type == 2)
    {
        *w = 1280;
        *h = 1024;
    }
    else if (type == 1 && (vga_canvas_current == vga_canvas_320_180 ||
                           vga_canvas_current == vga_canvas_640_360))
    {
        *w = 1280;
        *h = 720;
    }
    else
    {
        *w = 640;
        *h = 480;
    }
}

/* Channel 0's CANVAS and MODE as the VGA takes them. A canvas the VGA does
 * not know it naks and ignores. Going back to the console sizes the
 * console. Mode 0 sizes the terminal of the canvas's width, when it fits:
 * the VGA works that out with the same mode0_place. */
void vga_xreg(uint8_t index, uint16_t word)
{
    if (index < 8)
        vga_xregs[index] = word;
    if (index == 0)
    {
        if (word < VGA_CANVAS_COUNT)
        {
            bool to_console = word == vga_canvas_console &&
                              vga_canvas_current != vga_canvas_console;
            vga_canvas_current = (vga_canvas_t)word;
            if (to_console && vga_connected())
                cells_set_height(80, vga_console_rows(vga_display_sent));
        }
        memset(vga_xregs, 0, sizeof vga_xregs);
    }
    else if (index == 1)
    {
        int w, h;
        int16_t begin, end;
        uint8_t cols, rows;
        if (word == 0 && vga_connected() && vga_canvas_current != vga_canvas_console &&
            vga_canvas_geometry(vga_canvas_current, &w, &h) &&
            mode0_place((int16_t)w, (int16_t)h, vga_xregs, &begin, &end, &cols, &rows))
            cells_set_height(cols, rows);
        memset(vga_xregs, 0, sizeof vga_xregs);
    }
}

bool HOST_IN_RAM_FUNC vga_connected(void)
{
    return vga_state == VGA_CONNECTED ||
           vga_state == VGA_NO_VERSION;
}

int HOST_IN_FLASH_FUNC vga_boot_response(char *buf, size_t buf_size, int state,
                                                              unsigned width)
{
    if (!vga_connected())
        return -1;
    return vga_status_response(buf, buf_size, state, width);
}

int HOST_IN_FLASH_FUNC vga_status_response(char *buf, size_t buf_size,
                                                                  int state, unsigned)
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

bool vga_check_display_type(uint8_t *v)
{
    return *v <= 2;
}

void vga_apply_display_type(uint8_t display_type, bool changed)
{
    (void)display_type;
    if (changed)
        vga_needs_reset = true;
}

int HOST_IN_FLASH_FUNC vga_display_type_response(char *buf,
                                                                              size_t buf_size,
                                                                              int state,
                                                                              unsigned width)
{
    (void)state;
    (void)width;
    snprintf(buf, buf_size, STR_SET_VGA_RESPONSE,
             vga_get_display_type(), vga_get_display_type_verbose());
    return -1;
}

const char *vga_get_display_type_verbose(void)
{
    const char *const labels[] = {STR_VGA_DISPLAY_TYPE_0,
                                  STR_VGA_DISPLAY_TYPE_1,
                                  STR_VGA_DISPLAY_TYPE_2};
    return labels[vga_get_display_type()];
}
