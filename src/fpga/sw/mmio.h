/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's windows on the machine, as mapped in rp6502.sv and
 * rv_soc.sv. Byte windows by design; the register cells are true words.
 */

#ifndef _FPGA_SW_MMIO_H_
#define _FPGA_SW_MMIO_H_

#include <stdint.h>

#define MMIO_CONSOLE (*(volatile uint32_t *)0xF0000000u)
#define MMIO_KBD (*(volatile uint32_t *)0xF0000008u)
#define MMIO_PHI2 (*(volatile uint32_t *)0xF000000Cu)
#define MTIME_LO (*(volatile uint32_t *)0xF0000010u)
#define MTIME_HI (*(volatile uint32_t *)0xF0000014u)
#define MMIO_SLOT (*(volatile uint32_t *)0xF0000018u)
#define MMIO_HIDKEY (*(volatile uint32_t *)0xF000001Cu)
/* The controller and the dock's keyboard, as they stand. State, not
 * events: a pad's release is the absence of a bit, and a keyboard
 * report is a set whose order APF does not promise. */
#define MMIO_PAD_KEY (*(volatile uint32_t *)0xF0000020u)
#define MMIO_PAD_JOY (*(volatile uint32_t *)0xF0000024u)
#define MMIO_PAD_TRIG (*(volatile uint32_t *)0xF0000028u)
#define MMIO_KBD_KEY (*(volatile uint32_t *)0xF000002Cu)
#define MMIO_KBD_JOY (*(volatile uint32_t *)0xF0000030u)
#define MMIO_KBD_TRIG (*(volatile uint32_t *)0xF0000034u)
#define MMIO_MOU_KEY (*(volatile uint32_t *)0xF0000038u)
#define MMIO_MOU_JOY (*(volatile uint32_t *)0xF000003Cu)
#define MMIO_MOU_TRIG (*(volatile uint32_t *)0xF0000040u)

#define SRAM ((volatile uint8_t *)0x10000000u)
#define XRAM_WIN ((volatile uint8_t *)0x30000000u)
#define STAGE ((volatile const uint8_t *)0x60000000u)

/* The font asset sits in the last 64 KB of the staging store, above any
 * ROM the loader will ever be handed. The 64 KB below it is where the
 * host lands a Slot Read, since the bridge already knows how to write
 * the staging store and knows no other way to write us at all. Both
 * bounds are declared in data.json's size_maximum for slot 0, and so is
 * the 64 KB below them holding the OEM code page tables. */
#define FONTS ((volatile const uint8_t *)0x63FF0000u)
#define FILE_STAGE ((volatile const uint8_t *)0x63FE0000u)
#define FILE_STAGE_BRIDGE 0x03FE0000u
#define FILE_STAGE_SIZE 0x10000u
#define OEMCP ((volatile const uint8_t *)0x63FD0000u)
/* The most the host will move in one slot operation. */
#define FILE_XFER_MAX 0x8000u

/* The host's file bridge. FILE_WIN is one port of a block RAM whose
 * other port is the bridge's, so it is word-wide and write-only: the
 * machine fills it and the host reads it, for Open File's parameter
 * struct and for Slot Write's payload. FILE_WIN_BASE is where the host
 * sees it, and must agree with pocket_file's WINDOW_BASE. */
#define FILE_ID (*(volatile uint32_t *)0x80000000u)
#define FILE_OFFSET (*(volatile uint32_t *)0x80000004u)
#define FILE_LENGTH (*(volatile uint32_t *)0x80000008u)
#define FILE_BRIDGE (*(volatile uint32_t *)0x8000000Cu)
#define FILE_CTL (*(volatile uint32_t *)0x80000010u)
#define FILE_RESULT (*(volatile uint32_t *)0x80000014u)
#define FILE_WIN ((volatile uint32_t *)0x80001000u)
/* The interact menu's persisted settings, read-only. */
#define SET_PHI2 (*(volatile uint32_t *)0x80010000u)
#define SET_CP (*(volatile uint32_t *)0x80010004u)
#define FILE_WIN_BASE 0x20000000u
#define FILE_WIN_SIZE 512u

#define FILE_OP_READ 1u
#define FILE_OP_WRITE 2u
#define FILE_OP_OPEN 3u
#define FILE_OP_DT 4u
/* Get File answers into the staging store at FILE_BRIDGE, since the
 * bridge writes toward us and cannot write the window. */
#define FILE_OP_GETFILE 5u

#define FILE_ST_BUSY 0x01u
#define FILE_ST_ERR 0x0Eu
#define FILE_ST_TIMEOUT 0x10u
/* Halfwords the host wrote that have not reached the store yet. The
 * bridge reports a slot operation complete while its own queue is still
 * draining, so a read of what just arrived waits for this to clear. */
#define FILE_ST_DRAIN 0x20u

/* The terminal cell window is where the linker places term.c's screen
 * buffers; the register bank above it is the scanout seam. */
#define VID_ROW(i) (((volatile uint32_t *)0x50010000u)[i])
#define VID_CURSOR (*(volatile uint32_t *)0x50010080u)
#define VID_CURSOR_COLOR (*(volatile uint32_t *)0x50010084u)
#define VID_BLINK (*(volatile uint32_t *)0x50010088u)
#define VID_PROG (*(volatile uint32_t *)0x5001008Cu)
#define VID_FRAME (*(volatile uint32_t *)0x500100A0u)

/* The scanline program: per line per plane, the fill slot's
 * enable/mode/attr word and config pointer, then the sprite slot's
 * matching word and its count-over-config word; the canvas and
 * vsync-line registers sit above the table. */
#define VID_XPROG(line, plane, w) \
    (((volatile uint32_t *)0x50020000u)[(line) * 16 + (plane) * 4 + (w)])
/* The font store, a word at a time — byte lanes are what stop the fabric
 * inferring a block RAM, so every write is aligned and whole. One face
 * per 4 KB, in the store's own order. */
#define VID_FONT16 ((volatile uint32_t *)0x50040000u)
#define VID_FONT8 ((volatile uint32_t *)0x50041000u)
#define VID_ITALIC16 ((volatile uint32_t *)0x50042000u)
#define VID_FONT_DEC16 ((volatile uint32_t *)0x50043000u)
#define VID_CANVAS (*(volatile uint32_t *)0x50028000u)
#define VID_VSYNC_LINE (*(volatile uint32_t *)0x50028004u)
/* The render's lost races: sprite slots in the low half, plane fills
 * that missed the beam in the high. A write clears both. */
#define VID_OVERRUN (*(volatile uint32_t *)0x50028008u)
#define REGS_WIN ((volatile uint8_t *)0x20000000u)
#define UART_POP (*(volatile uint32_t *)0x20000040u)
#define RX_OFFER (*(volatile uint32_t *)0x20000048u)
#define AUD_PSG_XADDR (*(volatile uint32_t *)0x70000000u)
#define AUD_BEL_STRIKE (*(volatile uint32_t *)0x70000004u)
#define AUD_OPL_XADDR (*(volatile uint32_t *)0x70000008u)
#define CPU_RUN (*(volatile uint8_t *)0x40000000u)
#define API_PENDING (*(volatile uint8_t *)0x40000004u)

#endif /* _FPGA_SW_MMIO_H_ */
