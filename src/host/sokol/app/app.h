/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_SOKOL_APP_APP_H_
#define _HOST_SOKOL_APP_APP_H_

#include <stdbool.h>
#include <stdint.h>

struct sapp_event;

/* Seed the application from the launch options and report the window's initial
 * size in pixels: the canvas aspect at the requested scale plus the debugger's
 * menu strip, or the size the last debug session was left at when the debugger
 * is active and have_scale is false. Call before sokol starts. */
void app_prepare(uint32_t *fb, double scale, bool have_scale,
                 bool exit_on_halt, int *out_w, int *out_h);

/* --phi2 0: run without pacing. Each callback runs as many frames as one frame
 * period of wall time holds, so the window still answers the user while the
 * machine's time warps. Call before sokol starts. */
void app_set_unpaced(bool on);

/* The four sokol lifecycle callbacks each platform's sapp_desc points at. */
void app_init(void);
void app_frame(void);
void app_input(const struct sapp_event *e);
void app_cleanup(void);

/* The process exit code once sokol returns, which entry_run passes on: the
 * ROM's exit code when it halted the application outside debug mode, 1 when the
 * host closed the window on a running machine, and 0 otherwise. A console break
 * never reaches here, because app_break_leave does not return. */
int app_exit_code(void);

/* How this host stops a run from outside the program it is running: whether it
 * was asked for, and how to leave when it was. A host with no such thing, such
 * as an APK or a browser tab, installs neither and ends the run by closing its
 * window. */
void app_set_break(bool (*asked)(void), void (*leave)(void));

/* Boot a .rp6502, true on success. The path is host UTF-8 and is converted to
 * the guest's OEM code page here, so a platform passes what the OS handed it; a
 * spelling the code page cannot hold never boots, and a platform that has
 * another spelling (the Windows 8.3 name, say) should substitute it first.
 * Ignored while a DAP client owns the machine. A failed load leaves the machine
 * stopped, because rom_load streams records into live RAM before it can fail,
 * as on hardware where a failed LOAD leaves the CPU stopped in the monitor. */
bool app_boot_rom(const char *path);

uint64_t app_machine_ns(void);

/* sokol's logger, for every .logger.func. Its lines are the sokol log category,
 * and a panic (level 0) does not return. */
void app_log(const char *tag, uint32_t log_level, uint32_t log_item_id,
             const char *message_or_null, uint32_t line_nr,
             const char *filename_or_null, void *user_data);

#endif /* _HOST_SOKOL_APP_APP_H_ */
