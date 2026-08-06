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
/* The last data slot the host said it touched, and the size it named.
 * Bit 16 flags that anything was said at all, because slot 0 is a real
 * id. Written to clear. Zero forever on a platform with no such news. */
#define MMIO_UPD_ID (*(volatile uint32_t *)0xF0000044u)
#define MMIO_UPD_LEN (*(volatile uint32_t *)0xF0000048u)
/* How many slot announcements the host has made, counted on its own
 * clock before anything here can drop one. The id and length above
 * cross on a toggle, and two announcements closer together than that
 * synchroniser is deep cancel each other; this cannot. Watch the count,
 * read the payload after it moves. */
#define MMIO_UPD_N (*(volatile uint32_t *)0xF000004Cu)
/* The controller and the dock's keyboard. State, not events: a pad's
 * release is the absence of a bit, and a keyboard report is a set whose
 * order APF does not promise. The mouse below is the opposite — a
 * stamped report of deltas, counted rather than sampled. */
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

/* The fonts and the code page tables are whole files: the fabric reads
 * glyphs at random and the tables are looked up by codepoint, so neither
 * can be a window. They are sized to what they hold and nothing more.
 *
 * Everything a slot moves goes through a window instead, one per slot,
 * because the bridge writes the staging store and knows no other way to
 * write us at all. Thirty-two kilobytes each, which is the most any slot
 * operation carries. data.json declares all eleven addresses and must
 * agree with what is here. */
#define FONTS ((volatile const uint8_t *)0x63FEA000u)
#define OEMCP ((volatile const uint8_t *)0x63FE8000u)

#define SLOT_WIN_SIZE 0x8000u
#define SLOT_WIN(i) \
    ((volatile const uint8_t *)(0x63FA0000u + (uint32_t)(i) * SLOT_WIN_SIZE))
#define SLOT_WIN_BRIDGE(i) (0x03FA0000u + (uint32_t)(i) * SLOT_WIN_SIZE)
/* The chunk one slot operation moves, which is a whole window; a larger
 * file is several of them. */
#define FILE_XFER_MAX SLOT_WIN_SIZE

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
/* The interact menu's persisted settings, read-only, and the host's
 * clock. The UTC offset arrives in three pieces — hours, quarter hour,
 * and which side of Greenwich — because a menu list holds sixteen
 * options and the offset spans twenty-seven whole hours. RTC_EPOCH is
 * the Pocket's local wall time as seconds since 1970, written once at
 * core boot by host command 0x0090, and RTC_VALID says it ever was. */
#define SET_TZ_HOUR (*(volatile uint32_t *)0x80010008u)
#define RTC_EPOCH (*(volatile uint32_t *)0x8001000Cu)
#define RTC_VALID (*(volatile uint32_t *)0x80010010u)
#define SET_TZ_MIN (*(volatile uint32_t *)0x80010014u)
#define SET_TZ_WEST (*(volatile uint32_t *)0x80010018u)

/* The three pieces as the one number everything downstream wants:
 * minutes east of UTC. Defined once here because both the boot read
 * and the menu poll need it and they must not drift apart. */
static inline int32_t set_tz_minutes(void)
{
    int32_t m = (int32_t)((SET_TZ_HOUR & 0xFFu) * 60u + (SET_TZ_MIN & 0xFFu));
    return (SET_TZ_WEST & 1u) ? -m : m;
}
#define FILE_WIN_BASE 0x20000000u
#define FILE_WIN_SIZE 512u

#define FILE_OP_READ 1u
#define FILE_OP_WRITE 2u
#define FILE_OP_OPEN 3u
#define FILE_OP_DT 4u
/* The one command whose answer is a name rather than a number, and the
 * one direction the outbound window cannot carry: it lands wherever
 * FILE_BRIDGE points, so it takes a Slot Read's staging buffer. */
#define FILE_OP_GETFILE 5u
/* Analogue documents 0x0188 but never implemented it in its own
 * core_bridge_cmd.v; vendor/openfpga_rp6502 adds it. Its result codes
 * are not Open File's: 0 is written, 1 is slot not defined; 7 is the
 * override's own bridge deadline, a command no host ever picked up.
 * A 7 arrives as an ordinary answer, not as FILE_ST_TIMEOUT — the
 * bridge's deadline is deliberately the shorter one, so it retires
 * its own parked command while this side is still listening. The
 * timeout bit now means the bridge itself stopped answering. */
#define FILE_OP_FLUSH 6u

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
#define REGS_WIN ((volatile uint8_t *)0x20000000u)
#define UART_POP (*(volatile uint32_t *)0x20000040u)
#define RX_OFFER (*(volatile uint32_t *)0x20000048u)
#define AUD_PSG_XADDR (*(volatile uint32_t *)0x70000000u)
#define AUD_OPL_XADDR (*(volatile uint32_t *)0x70000008u)
/* The bell's voice, a channel block's seven bytes in its own order:
 * freq, duty, vol_attack / vol_decay, wave_release, pan_gate. */
#define AUD_BEL_LO (*(volatile uint32_t *)0x70000010u)
#define AUD_BEL_HI (*(volatile uint32_t *)0x70000014u)
#define CPU_RESB (*(volatile uint8_t *)0x40000000u)
#define API_PENDING (*(volatile uint8_t *)0x40000004u)

#endif /* _FPGA_SW_MMIO_H_ */
