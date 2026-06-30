/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Top level (sys.c): the 65C02 core, the one master clock, the 60 Hz frame
 * loop, frame presentation, exec, and PHI2. Every chips tick advances the
 * master clock by the PHI2 divider; the VGA scanlines and the s/ds/cs/ms timers
 * are deadline consumers of that same clock, so all timing is reproducible.
 */

#ifndef _EMU_SYS_H_
#define _EMU_SYS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The one master clock: the RP2350 system clock. Every 6502 (chips) tick
 * advances it by the PHI2 divider; the VGA scanlines and the s/ds/cs/ms timers
 * are deadline consumers of it, so all timing is reproducible. */
#define EMU_RP2350_KHZ 256000  /* 256 MHz sysclk (CPU_RP2350_KHZ) */
#define EMU_VGA_HZ 60          /* the RP6502 VGA is always 60 Hz */
#define EMU_VGA_SCANLINES 525  /* 640x480@60 total scanlines (480 visible + blanking) */

/* PHI2 (the 6502 clock) in kHz, derived from the 256 MHz master by the same
 * fractional divider as ria/sys/cpu.c. Configurable at runtime (atr API) and
 * via --phi2; reported value is the achievable one after quantization. */
#define EMU_PHI2_DEFAULT_KHZ 8000 /* CPU_PHI2_DEFAULT */
#define EMU_PHI2_MIN_KHZ 100
#define EMU_PHI2_MAX_KHZ 8000

void emu_set_phi2_khz(uint16_t khz); /* clamped to [MIN,MAX], quantized */
uint16_t emu_get_phi2_khz(void);

extern unsigned long emu_vga_frame_count; /* diagnostic: VGA frames run so far */

void emu_init(void);
void emu_run_frame(void);          /* run one 60 Hz VGA frame, rendering it scanline-by-scanline */
void emu_run_frame_norender(void); /* same, but skip pixel rendering (a catch-up frame) */

/* Frame presentation. emu_present_framebuffer() returns the completed frame the
 * window presents; emu_render copies that frame into fb for --screenshot / the
 * tests. */
const uint32_t *emu_present_framebuffer(void);
void emu_render(uint32_t *fb);

/* Request an exec: load rom_path (a host/drive path or overlay ROM name) as the
 * new program at the next frame boundary, restarting the CPU but keeping the
 * clock and the argv set by pro_api_exec. */
void emu_exec(const char *rom_path);

/* Set by the EXIT syscall; the CPU stops ticking once halted. */
extern bool emu_cpu_halted;
extern int emu_exit_code;

/* Deterministic virtual microsecond clock — the master clock all timing
 * derives from. It is a pure function of the scanline counter (see sys.c), so
 * the same number of frames always yields the same time: reproducible. */
uint64_t emu_now_us(void);

/* The live 65C02 instance, for the debugger UI + DAP register access (the debug
 * code casts to m6502_t*, which includes the chip header, so this need not pull
 * it in). */
void *sys_cpu(void); /* m6502_t* */

/* Optional per-CPU-cycle observer for the debugger UI. Display-only and MUST NOT
 * gate the CPU — dbg.c is the one authoritative engine. NULL when no observer is
 * registered. */
extern void (*emu_dbg_cycle_cb)(uint64_t pins);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_SYS_H_ */
