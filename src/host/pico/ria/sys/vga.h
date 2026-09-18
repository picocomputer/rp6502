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

#define VGA_BACKCHANNEL_PIN COM_UART_TX_PIN
#define VGA_BACKCHANNEL_BAUDRATE 115200
#define VGA_BACKCHANNEL_PIO pio1
#define VGA_BACKCHANNEL_SM 2

// The VSYNC trap is a PIO program that, once armed, raises an interrupt at the
// next high-to-low edge on the backchannel pin. It is not on pio1 with the
// receiver above, because pio1 has no instruction memory left. A PIO reads a
// GPIO input whatever the pin's function select is, so the trap waits on the
// same pin as the receiver.
#define VGA_VSYNC_TRAP_PIO pio0
#define VGA_VSYNC_TRAP_SM 3
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
// complete the pull it stalls on, plus 68 cycles of vga_backchannel_rx. Both
// sides run 8 PIO cycles per bit at 115200 baud, so 69 cycles is 74.9 us.
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

// Fully connected with backchannel.
bool vga_connected(void);

// Responders for status.
int vga_boot_response(char *buf, size_t buf_size, int state, unsigned width);
int vga_status_response(char *buf, size_t buf_size, int state, unsigned width);

// Configuration setting VGA
bool vga_check_display_type(uint8_t *v);
void vga_apply_display_type(uint8_t display_type, bool changed);
int vga_display_type_response(char *buf, size_t buf_size, int state, unsigned width);
bool vga_set_display_type(uint8_t display_type);
const char *vga_get_display_type_verbose(void);

void vga_set_canvas(uint16_t canvas_word);

void vga_set_tel_console_active(bool active);

#define VGA_CONFIG_DISPLAY_TYPE CONFIG_INT(D, vga, display_type, uint8_t, 0, \
    vga_check_display_type, vga_apply_display_type, STR_VGA, \
    vga_display_type_response, STR_HELP_SET_VGA, NULL)
/* VGA_DRIVER comes after RIA_DRIVER and PIX_DRIVER in RP6502_MACH_DRIVERS
 * because vga_init sends PIX messages and waits for the VGA's reply, and the
 * PIX transmitter shifts only on edges of the PHI2 clock that ria_init
 * starts. */
#define VGA_DRIVER DRIVER(vga_init, vga_task, nul_task, vga_run, vga_stop, vga_break, \
    VGA_CONFIG_DISPLAY_TYPE, nul_config, nul_sst)

#endif /* _RIA_SYS_VGA_H_ */
