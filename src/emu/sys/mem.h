/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared machine memory: the 64KB 6502 RAM, the shared 64KB XRAM, the RIA
 * register file (a window into RAM at $FFE0) and XSTACK, and the XRAM
 * write-notify ring. Plus the fixed bus geometry (the framebuffer size and the
 * RIA/VIA register windows). Owned by mem.c; nearly every module reads these.
 */

#ifndef _EMU_MEM_H_
#define _EMU_MEM_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

#define EMU_FB_WIDTH 640
#define EMU_FB_HEIGHT 480

/* RIA register window ($FFE0-$FFF9). Vectors $FFFA-$FFFF stay in RAM. */
#define RIA_WINDOW_LO 0xFFE0
#define RIA_WINDOW_HI 0xFFF9 /* inclusive */

/* The 6522 VIA is a physical chip on the bus at $FFD0-$FFDF (16 registers). */
#define VIA_WINDOW_LO 0xFFD0
#define VIA_WINDOW_HI 0xFFDF /* inclusive */

/* 64KB 6502 RAM and the shared 64KB extended RAM. */
extern uint8_t ram[0x10000];

/* The RIA register file is the top of RAM ($FFE0-$FFFF): regs is a view into
 * ram[], so a register write and a RAM write to that address are one and the
 * same. Mirror of ria/sys/mem.h register aliasing: only the low 5 bits matter. */
extern volatile uint8_t *const regs;
#define REGS(addr) regs[(addr) & 0x1F]
#define REGSW(addr) (*(uint16_t *)&REGS(addr))
extern uint8_t xram[0x10000];

/* xstack: 512 bytes top-down + 1 guard zero byte. Empty when ptr == SIZE. */
#define XSTACK_SIZE 0x200
extern uint8_t xstack[XSTACK_SIZE + 1];
extern size_t xstack_ptr;

/* XRAM write-notify ring (mirrors ria/sys/mem.h). A windowed XRAM write whose
 * page matches xram_queue_page records (low byte, value) here; the active
 * audio device's sample handler drains it to spot register changes (PSG gate
 * edges / OPL register writes). The vendored psg.c/opl.c reach these through
 * the sys/mem.h shim. */
extern volatile uint8_t xram_queue_page;
extern volatile uint8_t xram_queue_head;
extern volatile uint8_t xram_queue_tail;
extern volatile uint8_t xram_queue[256][2];

#ifdef __cplusplus
}
#endif

#endif /* _EMU_MEM_H_ */
