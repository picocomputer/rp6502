/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The application: what happens between sokol calling us and the machine having
 * run. A platform's entry.c seeds it with app_prepare, builds a sapp_desc around
 * the four callbacks, and takes app_exit_code back out.
 */

#ifndef _HOST_SOKOL_APP_APP_H_
#define _HOST_SOKOL_APP_APP_H_

#include <stdbool.h>
#include <stdint.h>

struct sapp_event;

/* Seed the application from the launch options and report the window's initial
 * pixel size (canvas aspect at the requested scale, plus the debugger menu
 * strip). Called before sokol starts. */
void app_prepare(uint32_t *fb, double scale, bool have_scale,
                 bool exit_on_halt, int *out_w, int *out_h);

/* The four sokol lifecycle callbacks each platform's sapp_desc points at. */
void app_init(void);
void app_frame(void);
void app_input(const struct sapp_event *e);
void app_cleanup(void);

/* The process exit code once sokol returns: the ROM's exit code when it halted
 * the app outside debug mode, else 0. entry_run returns this. */
int app_exit_code(void);

/* Boot a .rp6502 (rom_load + cold boot + fresh argv), true on success. The path
 * is host UTF-8; conversion to the guest's OEM code page happens here, so
 * platforms pass what the OS handed them (a lossy spelling never boots —
 * pre-substitute one that converts, like the Windows 8.3 fallback). Ignored
 * while a debug session owns the machine. A failed load halts the machine:
 * rom_load streams into live RAM before it can fail, so the old program may
 * already be clobbered — matching hardware, where a failed LOAD leaves the CPU
 * stopped in the monitor. */
bool app_boot_rom(const char *path);

/* Wall time the machine has spent running frames, in total. */
uint64_t app_machine_ns(void);

#endif /* _HOST_SOKOL_APP_APP_H_ */
