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

#define EMU_VGA_HZ 60         /* the RP6502 VGA is always 60 Hz */
#define EMU_VGA_SCANLINES 525 /* 640x480@60 total scanlines (480 visible + blanking) */

extern unsigned long emu_vga_frame_count; /* diagnostic: VGA frames run so far */

void emu_init(void);
void emu_run_frame(void);          /* run one 60 Hz VGA frame, rendering it scanline-by-scanline */
void emu_run_frame_norender(void); /* same, but skip pixel rendering (a catch-up frame) */

/* Request an exec: load rom_path (a host/drive path or overlay ROM name) as the
 * new program at the next frame boundary, restarting the CPU but keeping the
 * clock and the argv set by pro_api_exec. */
void emu_exec(const char *rom_path);

/* Set by the EXIT syscall; the CPU stops ticking once halted. */
extern bool emu_cpu_halted;
extern int emu_exit_code;

#ifdef __cplusplus
}
#endif

#endif /* _EMU_SYS_H_ */
