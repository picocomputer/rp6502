/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator's runner — the counterpart to ria/main.c. Cold boot (main_init),
 * the run/stop lifecycle, and the syscall op registry (main_api) the shared
 * ria/api/api.c dispatches through via "main.h".
 *
 * The machine itself — the bus, the system clock and the frame engine — is
 * sys/sys.c; each chip's tick lives with the chip (sys/cpu.c, emu/via.c, sys/ria.c,
 * sys/mem.c).
 */

#ifndef _EMU_MAIN_H_
#define _EMU_MAIN_H_

#include <stdbool.h>
#include <stdint.h>

#include "ria/main.h"

void main_init(void); /* cold boot: fan out to every subsystem */

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
