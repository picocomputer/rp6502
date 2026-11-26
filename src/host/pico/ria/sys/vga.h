/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_VGA_H_
#define _RIA_SYS_VGA_H_

/* Communications with RP6502-VGA.
 */

#include "core/vga/vga.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define VGA_BACKCHANNEL_PIN MANIFOLD_UART_TX_PIN
#define VGA_BACKCHANNEL_BAUDRATE 115200
#define VGA_BACKCHANNEL_PIO pio0
#define VGA_BACKCHANNEL_SM 3
#define VGA_BACKCHANNEL_IRQ PIO0_IRQ_1

// The VSYNC trap is IRQ flag 0 of the backchannel receiver, which raises it on
// every start bit. Armed, the flag is an interrupt source, so the next start
// bit raises VGA_VSYNC_TRAP_IRQ.
#define VGA_VSYNC_TRAP_IRQ PIO0_IRQ_0

// The trap is armed at VGA_VSYNC_WINDOW_US before the expected VSYNC and
// disarmed at the same time after it, unless it fires first. Until the VGA
// sends the VSYNC byte, it sends no ACK or NAK within RIA_VSYNC_LOCKOUT_US of
// the expected VSYNC, so this window must stay inside that span by more than
// one byte time plus the timestamp skew between the boards.
#define VGA_VSYNC_PERIOD_US 16667
#define VGA_VSYNC_WINDOW_US 1500

// VGA_VSYNC_TRANSIT_US is the time from the VGA queuing a byte to the autopush
// of that byte on the RIA: 1 PIO cycle for the idle VGA transmitter to
// complete the pull it stalls on, plus 136 cycles of vga_backchannel_rx. At
// 115200 baud the VGA runs 8 PIO cycles per bit and the RIA 16, so that is
// 1.1 us plus 73.8 us, 74.9 us.
// Subtracting it from the time a VSYNC byte is handled gives the time the VGA
// queued it.
#define VGA_VSYNC_TRANSIT_US 75

/* Main events
 */

void vga_init(void);
void vga_task(void);
void vga_run(void);
void vga_stop(void);
void vga_break(void);
void vga_flashing(void);

// Fully connected with backchannel.
bool vga_connected(void);

/* The gate every message of the RIA's main loop passes on PIX (pix_ready),
 * so all it sends the VGA is one paced stream: the console's bytes, DISPLAY
 * and CODE_PAGE, a 6502 program's xregs, the pointer and the selection, the
 * link's own messages and the RIA's own writes to XRAM. Only the 6502's
 * writes to XRAM, which core 1 and the stream DMA put on PIX, go round it.
 * It is shut while an answer is awaited (below), which the VGA gives once
 * the work the message made is done, and for a while after a message that
 * starts a job on more than one row (draw_job_started), which the VGA does a
 * few rows a pass of its main loop. An answer whose time is up is a timeout
 * here, which loses the VGA. With no VGA connected it is open. */
bool vga_gate_open(void);

/* Room on PIX for a byte of the console of a connected VGA: the gate is open,
 * no code page is still to go, and the byte before it has had its time at
 * the speed the console's terminal keeps (term_speed). */
bool vga_console_ready(void);

// Responders for status.
int vga_boot_response(char *buf, size_t buf_size, int state, unsigned width);
int vga_status_response(char *buf, size_t buf_size, int state, unsigned width);

// Configuration setting VGA
bool vga_check_display_type(uint8_t *v);
void vga_apply_display_type(uint8_t display_type, bool changed);
int vga_display_type_response(char *buf, size_t buf_size, int state, unsigned width);
uint8_t vga_get_display_type(void);
bool vga_set_display_type(uint8_t display_type);
const char *vga_get_display_type_verbose(void);

/* A register of channel 0 as it is sent to the VGA: the canvas and mode
 * xregs, which the console's terminal follows. */
void vga_xreg(uint8_t index, uint16_t word);

/* The messages the VGA answers on the backchannel, with an ack or, when it
 * refuses one, a nak: CANVAS and MODE on channel 0, and DISPLAY, CODE_PAGE
 * and FLASH_PROGRAM on channel 15. The VGA answers once the work the message
 * made is done: a job of its terminal, a new view and the clock it needs.
 * The answers carry no tag, so one message is awaited at a time, and the
 * gate sends the VGA nothing more until its answer comes. A VGA that is
 * connected and does not answer in time is lost, as a VGA that stops its
 * VSYNCs is.
 *
 * DISPLAY and CODE_PAGE are vga.c's own: it sends each as soon as the gate
 * opens, and nothing else is asked until they are gone. A 6502 program's
 * CANVAS and MODE, and FLASH's FLASH_PROGRAM, are asked here, and the answer
 * stays for the one who asked until it asks again. */
typedef enum
{
    VGA_ANSWER_ACK,
    VGA_ANSWER_NAK,
    VGA_ANSWER_TIMEOUT,
    VGA_ANSWER_WAITING,
} vga_answer_t;

/* The VGA's answer comes after the work the message made and out of the
 * window around each VSYNC, so this must exceed both plus a frame of
 * scheduling slack. The most work is a code page's full RIS: both sizes'
 * screens and alternate screens, 4 jobs of up to 32 rows, at TERM_JOB_ROWS
 * 11 a pass, 12 passes, about 2 ms at the 0.93 ms termload measures for a
 * whole-screen job at both sizes (vga.c's VGA_JOB_HOLD_US). A DISPLAY's new
 * view sets the clock in one of those passes. The window is the VGA's
 * RIA_VSYNC_LOCKOUT_US either side, an answer held up to 4.1 ms, and the
 * byte is 0.1 ms on the wire: about 6.2 ms and the clock's settling, with
 * over a frame, 16.7 ms, to spare. */
#define VGA_ANSWER_MS 25

/* The gate is open and nothing of vga.c's own is still to go, so a message
 * may be asked, or, as with no VGA connected, nothing will be sent. */
bool vga_ask_ready(void);

/* Sends a message the VGA answers, once vga_ask_ready, and awaits its answer
 * for up to timeout_ms. A VGA that is not connected is sent nothing, and this
 * is false. */
bool vga_ask(uint8_t ch4, uint8_t byte, uint16_t word, uint32_t timeout_ms);

/* The answer to the message vga_ask sent last. */
vga_answer_t vga_answer(void);

/* FLASH of VGA firmware takes the VGA, polled until it is not WAITING: ACK
 * once it is taken, TIMEOUT when no VGA is there. A VGA lost to a message it
 * did not answer is linked again, without the DISPLAY and CODE_PAGE that it
 * may not answer, and taken at its first VSYNC. From then on vga.c sends
 * nothing it awaits an answer to. */
vga_answer_t vga_flash_take(void);

#define VGA_CONFIG_DISPLAY_TYPE CONFIG_INT(D, vga, display_type, uint8_t, 0, \
    vga_check_display_type, vga_apply_display_type, STR_VGA, \
    vga_display_type_response, STR_HELP_SET_VGA, NULL)
/* VGA_DRIVER comes after RIA_DRIVER and PIX_DRIVER in RP6502_MACH_DRIVERS
 * because vga_init sends PIX messages and waits for the VGA's reply, and the
 * PIX transmitter shifts only on edges of the PHI2 clock that ria_init
 * starts. vga_init also starts the console's terminal (vterm.h),
 * which follows the VGA's. */
#define VGA_DRIVER DRIVER(   \
    INIT(vga_init)           \
    TASK_PUMPED(vga_task)    \
    RUN(vga_run)             \
    STOP(vga_stop)           \
    BREAK(vga_break)         \
    FLASHING(vga_flashing)   \
    VGA_CONFIG_DISPLAY_TYPE)

#endif /* _RIA_SYS_VGA_H_ */
