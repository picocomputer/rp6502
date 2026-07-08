/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_SYS_H_
#define _EMU_SYS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define SYS_VGA_HZ 60         /* the RP6502 VGA is always 60 Hz */
#define SYS_VGA_SCANLINES 525 /* 640x480@60 total scanlines (480 visible + blanking) */

/* Diagnostic: total VGA frames run so far (advances at 60 Hz). */
unsigned long sys_frame_count(void);

void sys_init(void);
void sys_run_frame(void);          /* run one 60 Hz VGA frame, rendering it scanline-by-scanline */
void sys_run_frame_norender(void); /* same, but skip pixel rendering (a catch-up frame) */

/* Request an exec: load rom_path (a host/drive path or overlay ROM name) as the
 * new program at the next frame boundary, restarting the CPU but keeping the
 * clock and the argv set by pro_api_exec. */
void sys_exec(const char *rom_path);

/* Program exit code, set by the EXIT syscall (and a failed exec). The CPU-halt
 * gate that stops ticking is cpu_halted() / cpu_set_halted() in sys/cpu.h. */
int sys_exit_code(void);
void sys_set_exit_code(int code);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_SYS_H_ */
