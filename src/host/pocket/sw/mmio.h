/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's windows on the machine, as mapped in rp6502.sv and
 * soc.sv. Byte windows by design; the register cells are true words.
 */

#ifndef _HOST_POCKET_SW_MMIO_H_
#define _HOST_POCKET_SW_MMIO_H_

#include <stdint.h>

#define MMIO_CONSOLE (*(volatile uint32_t *)0xF0000000u)
#define MMIO_KBD (*(volatile uint32_t *)0xF0000008u)
#define MMIO_PHI2 (*(volatile uint32_t *)0xF000000Cu)
#define MTIME_LO (*(volatile uint32_t *)0xF0000010u)
#define MTIME_HI (*(volatile uint32_t *)0xF0000014u)
#define MMIO_SLOT (*(volatile uint32_t *)0xF0000018u)
#define MMIO_HIDKEY (*(volatile uint32_t *)0xF000001Cu)
/* Bit 16 flags that anything was said at all, because slot 0 is a real
 * id. Written to clear. */
#define MMIO_UPD_ID (*(volatile uint32_t *)0xF0000044u)
#define MMIO_UPD_LEN (*(volatile uint32_t *)0xF0000048u)
/* The id and length above cross on a toggle, so two announcements closer
 * together than that synchroniser is deep cancel each other. This count
 * cannot: watch it, and read the payload after it moves. */
#define MMIO_UPD_N (*(volatile uint32_t *)0xF000004Cu)
/* Four controller slots, three words each. State, not events. A mouse is
 * the exception, counting stamped delta reports in the same word another
 * slot puts buttons in; the type nibble says which. See apf.c. */
#define MMIO_CONT_KEY(s) (*(volatile uint32_t *)(0xF0000050u + (s) * 12u))
#define MMIO_CONT_JOY(s) (*(volatile uint32_t *)(0xF0000054u + (s) * 12u))
#define MMIO_CONT_TRIG(s) (*(volatile uint32_t *)(0xF0000058u + (s) * 12u))
#define MMIO_CONT_SLOTS 4

#define SRAM ((volatile uint8_t *)0x10000000u)
#define XRAM_WIN ((volatile uint8_t *)0x30000000u)
#define STAGE ((volatile const uint8_t *)0x60000000u)

/* data.json's list order, its slot ids, and these addresses climb
 * together and must be changed together; data.json is strict JSON and
 * cannot carry this comment. The ROM is first because a hot reload
 * writes the new image through the primary slot record. Get File's
 * scratch and the savestate blob are the two addresses here data.json
 * does not declare, since neither is a slot, and the layouts sit above
 * the scratch page so the slots stay in one ascending run. ROM_MAX is
 * data.json's size_maximum. stage_map_gate.py checks all of it. */
#define ROM_IMG ((volatile const uint8_t *)0x60000000u)
#define ROM_BRIDGE 0x00000000u
#define ROM_MAX 0x03F00000u
/* The savestate blob, in the gap the ROM's ceiling gave up for it. The
 * host writes a blob here as ordinary bridge writes before it asks for
 * the load, so it arrives in the staging store and the engine reads it
 * back through the window like any other staged image. Reads of this
 * range are the one thing the host takes out of the machine, and
 * nothing here answers them: the engine stops the machine and reads its
 * memories through its own bus, and pocket_sst holds one word ready
 * ahead of the host. The firmware carries none of it either way. */
#define SST_BLOB_BRIDGE 0x03F00000u
#define SST_BLOB ((volatile const uint8_t *)0x63F00000u)
#define SST_BLOB_MAX 0x000A0000u
#define SLOT_WIN_SIZE 0x8000u
/* By descriptor, not by slot id: descriptor d is slot 1 + d, and its
 * window is the d'th above the ROM's ceiling. */
#define SLOT_WIN(d) \
    ((volatile const uint8_t *)(0x63FA0000u + (uint32_t)(d) * SLOT_WIN_SIZE))
#define SLOT_WIN_BRIDGE(d) (0x03FA0000u + (uint32_t)(d) * SLOT_WIN_SIZE)
#define FILE_XFER_MAX SLOT_WIN_SIZE
#define FONTS ((volatile const uint8_t *)0x63FE0000u)
#define OEMCP ((volatile const uint8_t *)0x63FEF000u)
#define GETFILE_WIN ((volatile const uint8_t *)0x63FF1000u)
#define GETFILE_BRIDGE 0x03FF1000u
#define KBDLAY ((volatile const uint8_t *)0x63FF2000u)

/* FILE_WIN is one port of a block RAM whose other port is the bridge's,
 * so it is word-wide and write-only. FILE_WIN_BASE is where the host
 * sees it, and must agree with pocket_file's WINDOW_BASE. */
#define FILE_ID (*(volatile uint32_t *)0x80000000u)
#define FILE_OFFSET (*(volatile uint32_t *)0x80000004u)
#define FILE_LENGTH (*(volatile uint32_t *)0x80000008u)
#define FILE_BRIDGE (*(volatile uint32_t *)0x8000000Cu)
#define FILE_CTL (*(volatile uint32_t *)0x80000010u)
#define FILE_RESULT (*(volatile uint32_t *)0x80000014u)
#define FILE_WIN ((volatile uint32_t *)0x80001000u)

/* Sleep and savestates. This one word is the whole of what the firmware
 * has to do with them.
 *
 * Nothing here mentions a create, because there is nothing to say: the
 * engine stops the soft CPU along with the rest of the machine, so no
 * firmware is running while a blob is made, and none is needed -- the
 * host is answered in fabric and the machine has itself back when the
 * last word has been read.
 *
 * A restore is told afterwards, because by then the firmware is the one
 * the blob brought. SST_RESTORED says so, and clearing it is what lets
 * the 6502 run again, so whatever fabric no blob carries is put back
 * first.
 *
 * SST_BLOB_SEEN is sticky from the first bridge write into the blob
 * window. It is how a boot learns it is a wake before any command
 * arrives, and it means: do not start the ROM, one is coming. */
#define SST_CTL (*(volatile uint32_t *)0x80020000u)

#define SST_RESTORED 0x01u
#define SST_BLOB_SEEN 0x02u
/* The host asked for a word the engine had not got to. It cannot
 * happen at the documented one-access-per-88-clocks; if it ever does,
 * the blob it took is short and the create answered an error. */
#define SST_UNDERRUN 0x04u
#define SST_RESTORE_ERR 0x08u
/* The interact menu's persisted settings, read-only. The UTC offset
 * arrives in three pieces because a menu list holds sixteen options and
 * the offset spans twenty-seven whole hours. SET_KB is a position in
 * def/keyboard.def plus one, so zero is a menu that has said nothing.
 * RTC_EPOCH is local wall time, written once at boot by 0x0090. */
#define SET_KB (*(volatile uint32_t *)0x80010000u)
#define SET_TZ_HOUR (*(volatile uint32_t *)0x80010008u)
#define RTC_EPOCH (*(volatile uint32_t *)0x8001000Cu)
#define RTC_VALID (*(volatile uint32_t *)0x80010010u)
#define SET_TZ_MIN (*(volatile uint32_t *)0x80010014u)
#define SET_TZ_WEST (*(volatile uint32_t *)0x80010018u)

/* Minutes east of UTC. Defined once because the boot read and the menu
 * poll both need it and must not drift apart. */
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
/* Answers with a name, landing wherever FILE_BRIDGE points. */
#define FILE_OP_GETFILE 5u
/* 0x0188 is documented but absent from core_bridge_cmd.v;
 * vendor/openfpga_rp6502 adds it. Result codes are not Open File's:
 * 0 written, 1 slot not defined, 7 the bridge's own deadline. The
 * bridge's deadline is deliberately shorter than ours, so a 7 arrives
 * as an ordinary answer and FILE_ST_TIMEOUT means the bridge stopped. */
#define FILE_OP_FLUSH 6u

#define FILE_ST_BUSY 0x01u
#define FILE_ST_ERR 0x0Eu
#define FILE_ST_TIMEOUT 0x10u
/* The bridge reports a slot operation complete while its own queue is
 * still draining, so a read of what just arrived waits for this. */
#define FILE_ST_DRAIN 0x20u
/* The host wrote into the response struct for this command. Get File
 * has no result code for "defined but nothing bound", so an answer of
 * ok with nothing written is legal and indistinguishable from a real
 * name -- except by this. */
#define FILE_ST_WROTE 0x40u

#define VID_ROW(i) (((volatile uint32_t *)0x50010000u)[i])
#define VID_CURSOR (*(volatile uint32_t *)0x50010080u)
#define VID_CURSOR_COLOR (*(volatile uint32_t *)0x50010084u)
#define VID_BLINK (*(volatile uint32_t *)0x50010088u)
#define VID_PROG (*(volatile uint32_t *)0x5001008Cu)
#define VID_FRAME (*(volatile uint32_t *)0x500100A0u)

/* Per line per plane: the fill slot's enable/mode/attr word and config
 * pointer, then the sprite slot's matching word and its count word. */
#define VID_XPROG(line, plane, w) \
    (((volatile uint32_t *)0x50020000u)[(line) * 16 + (plane) * 4 + (w)])
/* Word at a time: byte lanes stop the fabric inferring a block RAM, so
 * every write is aligned and whole. One face per 4 KB. */
#define VID_FONT16 ((volatile uint32_t *)0x50040000u)
#define VID_FONT8 ((volatile uint32_t *)0x50041000u)
#define VID_ITALIC16 ((volatile uint32_t *)0x50042000u)
#define VID_FONT_DEC16 ((volatile uint32_t *)0x50043000u)
#define VID_FONT_DEC8 ((volatile uint32_t *)0x50043200u)
#define VID_CANVAS (*(volatile uint32_t *)0x50028000u)
#define VID_VSYNC_LINE (*(volatile uint32_t *)0x50028004u)
#define REGS_WIN ((volatile uint8_t *)0x20000000u)
#define UART_POP (*(volatile uint32_t *)0x20000040u)
#define RX_OFFER (*(volatile uint32_t *)0x20000048u)
#define AUD_PSG_XADDR (*(volatile uint32_t *)0x70000000u)
/* Lets the PSG take a note-on from this firmware and not only from the
 * 6502, which is what a restore's replay of a channel block needs: the
 * gate bit rides in the same byte as the rest and the engine would
 * otherwise ignore it. Held over the replay and cleared after. */
#define AUD_PSG_REPLAY (*(volatile uint32_t *)0x70000004u)
#define AUD_OPL_XADDR (*(volatile uint32_t *)0x70000008u)
/* A channel block's seven bytes in order: freq, duty, vol_attack,
 * vol_decay, wave_release, pan_gate. */
#define AUD_BEL_LO (*(volatile uint32_t *)0x70000010u)
#define AUD_BEL_HI (*(volatile uint32_t *)0x70000014u)
#define CPU_RESB (*(volatile uint8_t *)0x40000000u)
#define API_PENDING (*(volatile uint8_t *)0x40000004u)

#endif /* _HOST_POCKET_SW_MMIO_H_ */
