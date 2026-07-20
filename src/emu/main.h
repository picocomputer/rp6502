/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator's runner — the counterpart to ria/main.c. Cold boot (main_init),
 * the frame/tick engine (main_run_frame), machine run state, and the syscall op
 * registry (main_api) the shared ria/api/api.c dispatches through via "main.h".
 */

#ifndef _EMU_MAIN_H_
#define _EMU_MAIN_H_

#include <stdbool.h>
#include <stdint.h>

#include "ria/main.h"

void main_init(void);               /* cold boot: fan out to every subsystem */
void main_run_frame(void);          /* run one 60 Hz VGA frame, rendering it */
void main_run_frame_norender(void); /* same, but skip pixel rendering (catch-up) */

unsigned long main_frame_count(void); /* diagnostic: total frames, advances at 60 Hz */

/* The virtual master clock in 1/8-of-a-256 MHz-tick units (2048/µs); the run loop
 * advances it, and pico/time.h's time_us_64 exposes it as the pico monotonic
 * microsecond clock. */
uint64_t main_clock_8(void);

/* Program exit code, set by the EXIT syscall (and a failed exec). The CPU-halt
 * gate that stops ticking is cpu_halted() / cpu_set_halted() in sys/cpu.h. */
int main_exit_code(void);
void main_set_exit_code(int code);

/* Point the op table's dir slots at the firmware FatFs handlers (fat, over the
 * RAM disk) or the emu's host handlers. */
void main_dir_ops_set(bool fat);

/* PIX XREG register dispatch: device 0 (RIA-local HID/audio), device 1 (VGA). */
bool main_xreg_0(uint8_t channel, uint8_t address, uint16_t word);
bool main_xreg_1(uint8_t channel, uint8_t address, uint16_t word);

#endif /* _EMU_MAIN_H_ */
