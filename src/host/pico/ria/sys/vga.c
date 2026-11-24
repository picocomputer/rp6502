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
#include "core/term/draw.h"
#include "core/vga/mode/mode0.h"
#include "core/term/vterm.h"
#include "ria.pio.h"
#include "machine.h"
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/sync.h>
#include <hardware/timer.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

// How long to wait for ACK to backchannel enable request
#define VGA_BACKCHANNEL_ACK_MS 3
/* At power on the VGA starts with the RIA and is ready later: it runs from
 * RAM, so it first copies itself there, and it builds its fonts and starts
 * its display before it listens to PIX. A request sent before then is lost,
 * since PIX has no handshake. On the bench the VGA first answers when the
 * RIA has been up about 60 ms, so the RIA keeps asking until it has been up
 * this long. A machine with no VGA waits all of it at every start. */
#define VGA_BOOT_MS 100
// The UART with nothing arriving for this long is idle: a byte takes 87 us.
#define VGA_UART_IDLE_MS 1
// How long to wait before aborting version string
#define VGA_VERSION_WATCHDOG_MS 3
// How many times the version is asked for, to have the same one twice
#define VGA_VERSION_TRIES 5
// How many times at least the VGA is asked for
#define VGA_REQUEST_TRIES 3
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

/* DISPLAY is still to go: after a connect, a stop, a break or a new display
 * type. */
static bool vga_needs_reset = true;
static vga_canvas_t vga_canvas_current = vga_canvas_console;

/* A code page still to go, and the console sends the VGA nothing until it
 * has gone. A page asked for before the last one went replaces it, since the
 * console sends nothing between them. */
static bool vga_code_page_owed;
static uint16_t vga_code_page;

/* What is awaited (vga.h), and when it is too late. vga_own_awaited is the
 * answer to DISPLAY or CODE_PAGE, from which vga.c takes nothing but its
 * coming, and vga_answer_state is the answer to the message vga_ask sent.
 * The backchannel's interrupt ends the wait. An answer is awaited only from a
 * connected VGA. */
static volatile bool vga_own_awaited;
static volatile vga_answer_t vga_answer_state = VGA_ANSWER_ACK;
static uint32_t vga_answer_deadline;

/* A job the console's terminal started (draw_job_started), which the VGA's
 * terminal starts at the same message and does TERM_JOB_ROWS rows a pass of
 * its main loop. The gate (vga.h) is shut until this time, so nothing comes
 * while the VGA still works on it. The hold is flat, the same for a job of
 * any size. Measured with termload (tests/hil), a whole-screen job at both
 * sizes takes the Pico VGA up to 0.93 ms under two 8-bit bitmaps and 0.67 ms
 * without. Set only while a VGA is connected. */
#define VGA_JOB_HOLD_US 1000
static uint64_t vga_job_hold_end_us;

/* The console sends the VGA no byte before this time, which keeps the speed
 * the console's terminal keeps (term_speed, DECSCS): a byte waits out the one
 * before it, 10 bits at the speed that byte went at, as SyncTERM's next
 * character does. A byte that went unlimited, or left it unlimited, sets no
 * wait, and the byte after it goes at the UART's pace, as every byte does at
 * 115200, whose 86 us are under the UART's own. Set only while a VGA is
 * connected. It is not the VGA at work, so a sync does not wait for it
 * (vga_console_busy). */
static uint64_t vga_console_paced_us;

/* Set when a flash of either chip begins, and never cleared: a flash ends in
 * a reboot or a lockup. From then on vga.c sends nothing it awaits, so
 * FLASH_PROGRAM's answer is the only one. */
static bool vga_flash_started;

/* The pointer as the tablet wants it, and what of it the VGA has yet. Its
 * position is sent once a frame at most, as POINTER_X and then POINTER_Y,
 * which moves it, so a mouse's hundreds of reports a second become a
 * frame's one move. A new shape goes on its own, in POINTER_SHAPE. What is
 * new waits while the gate is shut. */
static uint8_t vga_pointer_shape;
static int16_t vga_pointer_x;
static int16_t vga_pointer_y;
static bool vga_pointer_shape_dirty;
static bool vga_pointer_where_dirty;
static uint32_t vga_pointer_frame;

/* The frame the selection last went to the VGA in. A change of the
 * user's (vterm_selection.unsent) goes once a frame at most, as
 * SELECT_MARGINS, SELECT_START and then SELECT_END, which applies it, and only a change of
 * the user's goes: a change to its cells drops it on the VGA at the same
 * byte as here, since the RIA's terminal takes each byte of the console as
 * it sends it, ahead of the words. */
static uint32_t vga_select_frame;

static volatile uint32_t vga_vsync_deadline;
static uint8_t vga_vsync_frame;
/* The frames counted from the VSYNCs, with no 8-bit wrap, and when the last
 * one came: vga_frame_count. */
static volatile uint32_t vga_vsync_frames;
static volatile uint32_t vga_vsync_at_us;

/* FLASH linked a lost VGA again and awaits its first VSYNC, which moves the
 * VSYNC deadline from where the link put it. */
static bool vga_flash_linked;
static uint32_t vga_flash_link_deadline;

/* A flash has begun, or FLASH is linking the VGA again for one: vga.c sends
 * no DISPLAY, CODE_PAGE or selection. In RAM, for vga_task, which asks it
 * before it calls into flash. */
static inline bool HOST_IN_RAM_FUNC vga_silenced(void)
{
    return vga_flash_started || vga_flash_linked;
}

/* The frame number takes a new value, a step on or a correction. */
static __force_inline void vga_vsync_set(uint8_t frame)
{
    vga_vsync_frames += (uint32_t)(int32_t)(int8_t)(uint8_t)(frame - vga_vsync_frame);
    vga_vsync_at_us = time_us_32();
    vga_vsync_frame = frame;
    REGS(0xFFE3) = frame;
}

/* A VGA that stops sending VSYNCs is found lost only by vga_task, which a
 * loop on core 0 waiting for the console may not run, so the count goes on
 * by the clock once two frames pass with none: whatever waits on frames
 * still sees them go by. The count and its time are read as a pair no VSYNC
 * came between, and the clock after them: a wait that came out negative
 * would read as a VGA silent for an hour. */
unsigned long HOST_IN_RAM_FUNC vga_frame_count(void)
{
    uint32_t frames, at;
    do
    {
        frames = vga_vsync_frames;
        at = vga_vsync_at_us;
    } while (frames != vga_vsync_frames);
    uint32_t late = time_us_32() - at;
    if (late > 2 * VGA_VSYNC_PERIOD_US)
        frames += late / VGA_VSYNC_PERIOD_US;
    return frames;
}

static int vga_vsync_alarm = -1;
static bool vga_vsync_alarm_arms;
static volatile bool vga_vsync_trap_armed;
static volatile bool vga_vsync_trap_fired;

#define VGA_VERSION_MESSAGE_SIZE 64
static char vga_version_message[VGA_VERSION_MESSAGE_SIZE];

static inline void vga_pix_backchannel_disable(void)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x04, 0);
}

/* The console's terminal follows the VGA's (vterm.h). Every change
 * the VGA makes to its terminal is made here at the moment the RIA sends what
 * causes it. The console's bytes go to the VGA through the same PIX queue,
 * so the two terminals take them all in the same order. */

/* The display type the VGA was last sent, which sizes its console. */
static uint8_t vga_display_sent;

/* The canvas and mode registers of channel 0 as the VGA holds them. */
static uint16_t vga_xregs[8];

void HOST_IN_RAM_FUNC vga_set_pointer(uint8_t shape, int16_t x, int16_t y)
{
    if (shape != vga_pointer_shape)
    {
        vga_pointer_shape = shape;
        vga_pointer_shape_dirty = true;
    }
    if (x != vga_pointer_x || y != vga_pointer_y)
    {
        vga_pointer_x = x;
        vga_pointer_y = y;
        vga_pointer_where_dirty = true;
    }
}

static void HOST_IN_RAM_FUNC vga_pointer_task(void)
{
    if (!vga_connected() || !pix_ready())
        return;
    if (vga_pointer_where_dirty && vga_frame_count() != vga_pointer_frame)
    {
        vga_pointer_where_dirty = false;
        vga_pointer_frame = (uint32_t)vga_frame_count();
        pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x09, (uint16_t)vga_pointer_x);
        pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x0A, (uint16_t)vga_pointer_y);
    }
    if (vga_pointer_shape_dirty)
    {
        vga_pointer_shape_dirty = false;
        pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x08, vga_pointer_shape);
    }
}

/* The selection's words (vterm_select_words), from flash. */
static void HOST_IN_FLASH_FUNC vga_select_send(void)
{
    vterm_selection.unsent = false;
    vga_select_frame = (uint32_t)vga_frame_count();
    const vterm_select_words_t w = vterm_select_words();
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x0B, w.margins);
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x0C, w.start);
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x0D, w.end);
}

/* Nothing goes while a flash is under way, when nothing in flash may run.
 * The words wait while the gate is shut, and PIX keeps them in order with
 * the console's bytes. */
static void HOST_IN_RAM_FUNC vga_select_task(void)
{
    if (vga_connected() && !vga_silenced() && vterm_selection.unsent &&
        vga_frame_count() != vga_select_frame && pix_ready())
        vga_select_send();
}

/* The page goes to a connected VGA alone (vga_tell_code_page), and
 * vga_connect sends it again. */
void vga_set_code_page(uint16_t cp)
{
    vga_code_page = cp;
    vga_code_page_owed = vga_connected() && !vga_silenced();
}

/* The VGA's console program on a display, 32 rows on SXGA (2) and
 * otherwise 30, as the VGA places it. */
static void vga_place_console(uint8_t display)
{
    uint16_t xregs[8] = {0};
    int16_t begin, end;
    uint8_t cols, rows;
    if (mode0_place(640, display == 2 ? 512 : 480, xregs, &begin, &end, &cols, &rows))
        vterm_place(cols, rows, begin, (uint8_t)vga_term_row_px(640));
}

/* DISPLAY: the VGA goes back to the console canvas, which places the
 * console for the old display, places it for the new one, and does the
 * system's soft reset. */
static void vga_follow_display(uint8_t display)
{
    bool to_console = vga_canvas_current != vga_canvas_console;
    vterm_canvas(80, true);
    if (to_console)
        vga_place_console(vga_display_sent);
    vga_place_console(display);
    vterm_RIS_no_clear();
    vga_display_sent = display;
}

/* With no VGA the console is a terminal on the UART, and that terminal
 * answers the queries, so the console's terminal is quiet by default. */
static void vga_follow_none(void)
{
    vterm_RIS();
    vterm_unplace();
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
    vga_vsync_set((uint8_t)(vga_vsync_frame + 1));
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
        uint8_t next = vga_vsync_frame;
        if (frame < (next & 0xF))
            next += 0x10;
        vga_vsync_set((uint8_t)((next & 0xF0) | frame));
        ria_trigger_vsync();
        break;
    }
    /* One message is awaited at a time. The VGA refuses neither of vga.c's
     * own: it knows every display type the RIA sends, and it turns a code
     * page it lacks into one it has. */
    case 0x90:
    case 0xA0:
        if (vga_own_awaited)
            vga_own_awaited = false;
        else if (vga_answer_state == VGA_ANSWER_WAITING)
            vga_answer_state = (byte & 0xF0) == 0x90 ? VGA_ANSWER_ACK : VGA_ANSWER_NAK;
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
    if (!timeout && mbuf_len == 4 && !strncasecmp(STR_VGA1, (char *)buf1k, 4))
        vga_state = VGA_FOUND;
    else
        vga_state = VGA_NOT_FOUND;
}

// The line is idle when a read of one byte times out.
static bool vga_uart_idle;
static void vga_idle_callback(bool timeout)
{
    vga_uart_idle = timeout;
    vga_state = VGA_NOT_FOUND;
}

// Until the read in progress calls back.
static void HOST_IN_FLASH_FUNC vga_mbuf_wait(void)
{
    vga_state = VGA_TESTING;
    while (vga_state == VGA_TESTING)
    {
        manifold_uart_drain_rx();
        mbuf_task();
    }
}

/* One version off the backchannel into vga_version_message. Returns a hash
 * of it that is never 0, or 0 when the VGA stops sending before a whole one
 * came. A version is "VGA ", then printable characters, then a CR. A vsync,
 * an ACK or a NAK may come between its bytes. Anything else is noise, or
 * the end of a byte the line was part way through, and the read starts over
 * at the next V. Only a character that is kept gives the VGA more time, so
 * noise cannot hold the read. */
static uint32_t HOST_IN_FLASH_FUNC vga_read_version(void)
{
    static const char begins[] = "VGA ";
    size_t length = 0;
    uint32_t hash = 0;
    absolute_time_t timer = make_timeout_time_ms(VGA_VERSION_WATCHDOG_MS);
    while (!time_reached(timer))
    {
        if (pio_sm_is_rx_fifo_empty(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM))
            continue;
        uint8_t byte = pio_sm_get(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM) >> 24;
        if (byte >= 0x80 && byte < 0xB0)
        {
            vga_backchannel_command(byte);
            continue;
        }
        if (byte == '\r' && length > sizeof begins - 1)
            return hash | 1;
        if (byte < ' ' || byte > '~' || length == VGA_VERSION_MESSAGE_SIZE - 1u ||
            (length < sizeof begins - 1 && byte != begins[length]))
        {
            length = 0;
            hash = 0;
            if (byte != begins[0])
                continue;
        }
        else
            timer = make_timeout_time_ms(VGA_VERSION_WATCHDOG_MS);
        vga_version_message[length] = byte;
        vga_version_message[++length] = 0;
        hash = (hash ^ byte) * 16777619u; // FNV-1a
    }
    return 0;
}

/* The link goes: the backchannel and its VSYNC trap stop, and the pin goes
 * back to the UART. Nothing answers with the backchannel off, so what is
 * awaited is a timeout now, and an answer from before is never taken for one
 * to a message of the next connection. A code page still to go goes with
 * it, and vga_connect sends the page again. Nothing holds the gate for a
 * VGA that is gone. */
static void vga_unlink(void)
{
    vga_backchannel_irq_disable();
    vga_own_awaited = false;
    if (vga_answer_state == VGA_ANSWER_WAITING)
        vga_answer_state = VGA_ANSWER_TIMEOUT;
    vga_job_hold_end_us = 0;
    vga_code_page_owed = false;
    vga_vsync_alarm_arms = false;
    if (vga_vsync_alarm >= 0)
        timer_hw->armed = 1u << vga_vsync_alarm;
    vga_vsync_trap_disarm();
    vga_pix_backchannel_disable();
    gpio_set_function(VGA_BACKCHANNEL_PIN, GPIO_FUNC_UART);
    vga_state = VGA_CONNECTION_LOST;
}

/* The VGA is lost, and the console is a terminal on the UART again. In
 * flash, as the gate's timeout reaches it from the console's RAM path. */
static void HOST_IN_FLASH_FUNC vga_lose(void)
{
    vga_unlink();
    mon_add_response_utf8(S(STR_ERR_VGA_CONNECTION_LOST));
    vga_follow_none();
}

/* Something is awaited, its time up or not. */
static inline bool HOST_IN_RAM_FUNC vga_awaiting(void)
{
    return vga_own_awaited || vga_answer_state == VGA_ANSWER_WAITING;
}

/* An answer whose time is up is a timeout, which loses the VGA. Only a
 * connected VGA is awaited, and vga_unlink ends the wait of the VGA it lets
 * go, so the VGA lost here is connected. The interrupts are held off so an
 * answer that comes at that moment is not written over. */
static void HOST_IN_RAM_FUNC vga_answer_expire(void)
{
    if (!vga_awaiting() || (int32_t)(time_us_32() - vga_answer_deadline) < 0)
        return;
    uint32_t irqs = save_and_disable_interrupts();
    bool late = vga_awaiting();
    vga_own_awaited = false;
    if (vga_answer_state == VGA_ANSWER_WAITING)
        vga_answer_state = VGA_ANSWER_TIMEOUT;
    restore_interrupts(irqs);
    if (late)
        vga_lose();
}

/* Something is awaited, and its time is not up: one whose time is up is a
 * timeout first. */
static bool HOST_IN_RAM_FUNC vga_still_awaiting(void)
{
    vga_answer_expire();
    return vga_awaiting();
}

bool HOST_IN_RAM_FUNC vga_gate_open(void)
{
    return !vga_still_awaiting() && time_us_64() >= vga_job_hold_end_us;
}

/* A message goes once PIX takes it, past the gate, and its answer is
 * awaited, vga.c's own or the asker's. The gate is waited out first with the
 * interrupts on. The wait begins as the message goes, and the interrupts are
 * held off across both so the backchannel cannot take the answer before it
 * is awaited: it records an answer only while one is. */
static void vga_await(uint8_t ch4, uint8_t byte, uint16_t word,
                      uint32_t timeout_ms, bool own)
{
    while (!pix_ready())
        tight_loop_contents();
    uint32_t irqs = save_and_disable_interrupts();
    pix_send_blocking(PIX_DEVICE_VGA, ch4, byte, word);
    vga_answer_deadline = time_us_32() + timeout_ms * 1000;
    if (own)
        vga_own_awaited = true;
    else
        vga_answer_state = VGA_ANSWER_WAITING;
    restore_interrupts(irqs);
}

/* A code page still to go goes once the gate opens, and the console's
 * terminal takes it as the VGA's does, between the same two bytes. */
static void HOST_IN_FLASH_FUNC vga_tell_code_page(void)
{
    if (!vga_code_page_owed || !vga_gate_open())
        return;
    vga_code_page_owed = false;
    vga_await(0xF, 0x01, vga_code_page, VGA_ANSWER_MS, true);
    vterm_set_code_page(vga_code_page);
}

/* DISPLAY, which is still to go, goes once the gate opens and the code page
 * has gone. It resets the terminal, so everything printed before it goes out
 * first: an escape sequence cut in half by the reset would print the rest of
 * itself as text, and DISPLAY waits out the hold of a job the last of it
 * started. The VGA's DISPLAY goes back to the console canvas, and so does
 * the RIA. */
static void vga_tell_display(void)
{
    if (vga_silenced() || !vga_gate_open() || vga_code_page_owed)
        return;
    vga_needs_reset = false;
    if (vga_connected())
    {
        manifold_flush();
        uint8_t display = vga_get_display_type();
        vga_await(0xF, 0x00, display, VGA_ANSWER_MS, true);
        vga_follow_display(display);
    }
    vga_canvas_current = vga_canvas_console;
    memset(vga_xregs, 0, sizeof vga_xregs);
}

bool vga_ask_ready(void)
{
    vga_tell_code_page();
    return vga_gate_open() && !(vga_needs_reset && vga_connected() && !vga_silenced());
}

bool vga_ask(uint8_t ch4, uint8_t byte, uint16_t word, uint32_t timeout_ms)
{
    if (!vga_connected())
        return false;
    vga_await(ch4, byte, word, timeout_ms, false);
    return true;
}

vga_answer_t vga_answer(void)
{
    vga_answer_expire();
    return vga_answer_state;
}

/* The console of a connected VGA is held: a code page is still to go, or
 * the gate is shut. A code page goes here once the gate opens, and an answer
 * whose time is up is a timeout here, which loses the VGA, since a flush of
 * the console waits on nothing else. While the RIA rewrites its flash
 * nothing is awaited or still to go (vga_flashing), so the console never
 * calls into flash. */
static bool HOST_IN_RAM_FUNC vga_console_held(void)
{
    if (vga_code_page_owed)
        vga_tell_code_page();
    return vga_code_page_owed || !vga_gate_open();
}

bool HOST_IN_RAM_FUNC vga_console_ready(void)
{
    return !vga_console_held() && vga_connected() &&
           time_us_64() >= vga_console_paced_us && pix_ready();
}

bool vga_console_busy(void)
{
    return vga_connected() && vga_console_held();
}

/* A connected VGA whose VSYNCs stop is lost. One that FLASH linked again and
 * that never sent one is let go without a word, as FLASH says it is not
 * connected. */
static void HOST_IN_RAM_FUNC vga_vsync_expire(void)
{
    if (!vga_connected() || (int32_t)(time_us_32() - vga_vsync_deadline) < 0)
        return;
    if (vga_flash_linked)
        vga_unlink();
    else
        vga_lose();
}

/* The VGA is asked for and its backchannel brought up, which brings its
 * VSYNCs: the link alone, with nothing sent to its terminal. Only the UART,
 * the VGA's own link, is cleared and read, so a reconnect at the run after a
 * lost link keeps the keyboard and telnet type-ahead. False when no VGA
 * answers. */
static bool HOST_IN_FLASH_FUNC vga_link(void)
{
    vga_backchannel_irq_disable();
    const bool was_lost = vga_state == VGA_CONNECTION_LOST;

    /* Test if VGA connected. The request repeats a few times, so one answer
     * spoiled by noise is not the end of it, and at power on for as long as
     * a VGA that started with the RIA may still be on its way up. */
    for (unsigned asked = 1;; asked++)
    {
        com_rx_clear_source(COM_SOURCE_UART);
        mbuf_read(COM_SOURCE_UART, VGA_BACKCHANNEL_ACK_MS, vga_rln_callback, 4);
        vga_pix_backchannel_request();
        vga_mbuf_wait();
        if (vga_state == VGA_FOUND)
            break;
        /* An answer that began as the wait ended, or came behind a stray
         * byte, is still arriving. Asked again now, the VGA would answer
         * straight after it and every read would be as far out of step, so
         * the line goes idle first, or the time runs out on a line that
         * never does. What came is not something typed. */
        const absolute_time_t noisy = make_timeout_time_ms(VGA_BACKCHANNEL_ACK_MS);
        do
        {
            mbuf_read(COM_SOURCE_UART, VGA_UART_IDLE_MS, vga_idle_callback, 1);
            vga_mbuf_wait();
        } while (!vga_uart_idle && !time_reached(noisy));
        if (asked >= VGA_REQUEST_TRIES && time_us_64() >= VGA_BOOT_MS * 1000)
            break;
    }
    if (vga_state != VGA_FOUND)
    {
        /* A VGA that was lost is asked for again at the next run. */
        vga_state = was_lost ? VGA_CONNECTION_LOST : VGA_NOT_FOUND;
        com_rx_clear_source(COM_SOURCE_UART);
        vga_pix_backchannel_disable();
        gpio_set_function(VGA_BACKCHANNEL_PIN, GPIO_FUNC_UART);
        vga_follow_none();
        return false;
    }

    // Turn on the backchannel
    pio_gpio_init(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_PIN);
    // Drain bytes the PIO sampled off the pin before the VGA took it
    while (!pio_sm_is_rx_fifo_empty(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM))
        (void)pio_sm_get(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM);

    /* The version is asked for until one comes that has come before, since
     * a line that took in noise gives a different wrong one each time. The
     * state stays found until then, which keeps the vsync trap from arming
     * on a version byte's start bit. */
    uint32_t seen[VGA_VERSION_TRIES];
    bool same = false;
    for (unsigned asked = 0; asked < VGA_VERSION_TRIES && !same; asked++)
    {
        vga_pix_backchannel_enable();
        seen[asked] = vga_read_version();
        for (unsigned i = 0; seen[asked] && i < asked; i++)
            same |= seen[i] == seen[asked];
    }
    if (!same)
        vga_version_message[0] = 0;
    vga_state = same ? VGA_CONNECTED : VGA_NO_VERSION;

    /* A VGA that answered a request late answers the next one too, and the
     * second answer is not something typed. It is on the wire before the
     * version is, so it has all arrived by now. */
    com_rx_clear_source(COM_SOURCE_UART);

    // Enable backchannel IRQ now that version string phase is complete
    vga_backchannel_irq_enable();
    vga_vsync_deadline = time_us_32() + VGA_VSYNC_WATCHDOG_MS * 1000;
    return true;
}

static void HOST_IN_FLASH_FUNC vga_connect(void)
{
    if (!vga_link())
        return;

    /* The VGA may have kept its state while the RIA restarted, or started
     * fresh while the RIA ran. The code page and DISPLAY go again, with a hard
     * reset between them to both terminals, so the two start alike, in a
     * build that logs too. */
    vga_needs_reset = true;
    vga_pointer_shape_dirty = true;
    vga_pointer_where_dirty = true;
    if (oem_get_code_page_run()) /* at boot oem_init sends it */
        vga_set_code_page(oem_get_code_page_run());
    manifold_write_to(COM_TERM_SCREEN, STR_TERM_HARD_RESET, STR_TERM_HARD_RESET_LEN);
    com_set_reply_default(false);
    /* A VGA of the firmware before the RIA answered the queries still has
     * its own answers on; this turns them off, so an update of either chip
     * first works. A VGA since then ignores it. */
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x02, 1);
}

/* The console's terminal (vterm.h) follows the VGA's in text cells,
 * fed every byte the VGA's terminal gets, as it is sent, so the speed it
 * keeps is the VGA's too. Each byte holds the next one for its time at the
 * speed it went at (vga_console_paced_us). */
static cell_t vga_console_cells[VTERM_CELLS];

static int vga_console_out(const char *buf, int length, bool force)
{
    (void)force;
    for (int i = 0; i < length; i++)
    {
        uint32_t speed = term_speed(&vterm);
        term_write(&vterm, (uint8_t)buf[i]);
        if (speed && term_speed(&vterm) && vga_connected())
            vga_console_paced_us = time_us_64() + 10u * 1000000u / speed;
    }
    return length;
}

/* While the RIA rewrites its flash, the console's output path runs from RAM
 * and the terminal, in flash, is no longer fed. An answer still awaited is
 * waited out first, since its timeout runs code in flash, and nothing more is
 * sent that is awaited. */
void vga_flashing(void)
{
    manifold_set_term_out(NULL);
    while (vga_still_awaiting())
        tight_loop_contents();
    vga_flash_started = true;
    vga_code_page_owed = false;
}

/* The VGA draws the console, so the RIA has no blinks to start again. */
void draw_reset(void)
{
}

/* The VGA does the same job over passes of its main loop: the gate is shut
 * until it is surely done. A job started while an answer is awaited is the
 * work of the message awaited, which the RIA's terminal takes after sending
 * it, and the VGA answers once that work is done, so the answer is the wait. */
void draw_job_started(void)
{
    if (vga_connected() && !vga_awaiting())
        vga_job_hold_end_us = time_us_64() + VGA_JOB_HOLD_US;
}

void HOST_IN_FLASH_FUNC vga_init(void)
{
    // The terminal first, as connecting resets and sizes it.
    vterm_init(vga_console_cells);
    manifold_set_term_out(vga_console_out);

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
    vga_vsync_expire();
    vga_answer_expire();
    if (vga_code_page_owed)
        vga_tell_code_page();
    if (vga_needs_reset)
        vga_tell_display();
    vga_pointer_task();
    vga_select_task();
}

vga_answer_t HOST_IN_FLASH_FUNC vga_flash_take(void)
{
    if (vga_still_awaiting())
        return VGA_ANSWER_WAITING;
    if (!vga_connected())
    {
        vga_flash_linked = vga_state == VGA_CONNECTION_LOST && !vga_flash_linked &&
                           vga_link();
        if (!vga_flash_linked)
            return VGA_ANSWER_TIMEOUT;
        vga_flash_link_deadline = vga_vsync_deadline;
        return VGA_ANSWER_WAITING;
    }
    if (vga_flash_linked && vga_vsync_deadline == vga_flash_link_deadline)
        return VGA_ANSWER_WAITING;
    vga_flash_linked = false;
    vga_flash_started = true;
    vga_code_page_owed = false;
    return VGA_ANSWER_ACK;
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

int16_t vga_canvas_width(void)
{
    int w, h;
    vga_canvas_size(&w, &h);
    return (int16_t)w;
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
 * not know it naks and ignores. Going back to the console places the
 * console. Mode 0 places the terminal of the canvas's width, when it fits:
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
            if (vga_connected())
            {
                vterm_canvas((uint8_t)vga_term_columns(vga_canvas_width()),
                             word == vga_canvas_console);
                if (to_console)
                    vga_place_console(vga_display_sent);
            }
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
            vterm_place(cols, rows, begin, (uint8_t)vga_term_row_px(w));
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
