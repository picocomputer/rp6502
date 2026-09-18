/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_MMIO_H_
#define _HOST_POCKET_SW_MMIO_H_

#include <stdint.h>

#define MMIO_CONSOLE (*(volatile uint32_t *)0xF0000000u)
#define MMIO_PHI2 (*(volatile uint32_t *)0xF000000Cu)
#define MTIME_LO (*(volatile uint32_t *)0xF0000010u)
#define MTIME_HI (*(volatile uint32_t *)0xF0000014u)
#define MMIO_SLOT (*(volatile uint32_t *)0xF0000018u)
#define MMIO_HIDKEY (*(volatile uint32_t *)0xF000001Cu)
#define MMIO_UPD_ID (*(volatile uint32_t *)0xF0000044u)
#define MMIO_UPD_LEN (*(volatile uint32_t *)0xF0000048u)
#define MMIO_UPD_N (*(volatile uint32_t *)0xF000004Cu)
#define MMIO_CONT_KEY(s) (*(volatile uint32_t *)(0xF0000050u + (s) * 12u))
#define MMIO_CONT_JOY(s) (*(volatile uint32_t *)(0xF0000054u + (s) * 12u))
#define MMIO_CONT_TRIG(s) (*(volatile uint32_t *)(0xF0000058u + (s) * 12u))
#define MMIO_CONT_SLOTS 4

#define SRAM ((volatile uint8_t *)0x10000000u)
#define XRAM_WIN ((volatile uint8_t *)0x30000000u)
#define STAGE ((volatile const uint8_t *)0x60000000u)

/* These addresses must agree with the slots in data.json.
 * stage_map_gate.py checks the ROM, savestate blob, asset and Get File
 * addresses against it. ROM_MAX is the ROM slot's size_maximum. */
#define ROM_IMG ((volatile const uint8_t *)0x60000000u)
#define ROM_BRIDGE 0x00000000u
#define ROM_MAX 0x03F00000u
#define SST_BLOB_BRIDGE 0x03F00000u
#define SST_BLOB ((volatile const uint8_t *)0x63F00000u)
#define SST_BLOB_MAX 0x000A0000u
#define SLOT_WIN_SIZE 0x8000u
#define SLOT_WIN(d) \
    ((volatile const uint8_t *)(0x63FA0000u + (uint32_t)(d) * SLOT_WIN_SIZE))
#define SLOT_WIN_BRIDGE(d) (0x03FA0000u + (uint32_t)(d) * SLOT_WIN_SIZE)
#define FILE_XFER_MAX SLOT_WIN_SIZE
#define FONTS ((volatile const uint8_t *)0x63FE0000u)
#define OEMCP ((volatile const uint8_t *)0x63FEF000u)
#define GETFILE_WIN ((volatile const uint8_t *)0x63FF1000u)
#define GETFILE_BRIDGE 0x03FF1000u
#define KBDLAY ((volatile const uint8_t *)0x63FF2000u)

/* FILE_WIN is the write port of a dual-port RAM in pocket_file.sv. The
 * RAM's read port is on the bridge, so the firmware cannot read FILE_WIN
 * back, and the write port has no byte enables, so each write stores a
 * whole word. FILE_WIN_BASE is the bridge address of the same RAM and
 * must match WINDOW_BASE in pocket_file.sv. */
#define FILE_ID (*(volatile uint32_t *)0x80000000u)
#define FILE_OFFSET (*(volatile uint32_t *)0x80000004u)
#define FILE_LENGTH (*(volatile uint32_t *)0x80000008u)
#define FILE_BRIDGE (*(volatile uint32_t *)0x8000000Cu)
#define FILE_CTL (*(volatile uint32_t *)0x80000010u)
#define FILE_RESULT (*(volatile uint32_t *)0x80000014u)
#define FILE_WIN ((volatile uint32_t *)0x80001000u)

/* SST_RESTORED is set when a savestate load finishes, whether or not the
 * load succeeded, and SST_RESTORE_ERR is set beside it when the load
 * failed. After a successful load the 6502 stays in reset until the
 * firmware writes SST_RESTORED to SST_CTL, so the state that the blob does
 * not contain is put back before that write. SST_BLOB_SEEN is set by a
 * bridge write into the blob window and clears when the load finishes. */
#define SST_CTL (*(volatile uint32_t *)0x80020000u)

#define SST_RESTORED 0x01u
#define SST_BLOB_SEEN 0x02u
#define SST_UNDERRUN 0x04u
#define SST_RESTORE_ERR 0x08u
/* The UTC offset comes from three interact menu entries because a menu
 * list holds at most sixteen options and the offset spans twenty-seven
 * whole hours. SET_KB is a position in def/keyboard.def plus one, so zero
 * means that the host has not written a layout to it. */
#define SET_KB (*(volatile uint32_t *)0x80010000u)
#define SET_TZ_HOUR (*(volatile uint32_t *)0x80010008u)
#define RTC_EPOCH (*(volatile uint32_t *)0x8001000Cu)
#define RTC_VALID (*(volatile uint32_t *)0x80010010u)
#define SET_TZ_MIN (*(volatile uint32_t *)0x80010014u)
#define SET_TZ_WEST (*(volatile uint32_t *)0x80010018u)

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
#define FILE_OP_GETFILE 5u
/* Flush, 0x0188, is missing from Analogue's core_bridge_cmd.v and is
 * added in vendor/openfpga_rp6502. core_bridge_cmd ends a data slot
 * command with result 7 when the host has not finished it within about
 * 0.9 s, half of pocket_file's deadline, so FILE_ST_TIMEOUT after a write
 * to FILE_CTL means that core_bridge_cmd itself has stopped.
 * FILE_ST_TIMEOUT also reads set from the time the FPGA is configured
 * until the first write to FILE_CTL. */
#define FILE_OP_FLUSH 6u

#define FILE_ST_BUSY 0x01u
#define FILE_ST_ERR 0x0Eu
#define FILE_ST_TIMEOUT 0x10u
/* A slot operation can complete while the write FIFO in
 * pocket_bridge.sv still holds writes for the staging store, so a read of
 * the new data waits for this bit to clear. */
#define FILE_ST_DRAIN 0x20u
#define FILE_ST_WROTE 0x40u

#define VID_ROW(i) (((volatile uint32_t *)0x50010000u)[i])
#define VID_CURSOR (*(volatile uint32_t *)0x50010080u)
#define VID_CURSOR_COLOR (*(volatile uint32_t *)0x50010084u)
#define VID_BLINK (*(volatile uint32_t *)0x50010088u)
#define VID_PROG (*(volatile uint32_t *)0x5001008Cu)
#define VID_FRAME (*(volatile uint32_t *)0x500100A0u)

#define VID_XPROG(line, plane, w) \
    (((volatile uint32_t *)0x50020000u)[(line) * 16 + (plane) * 4 + (w)])
/* The font RAM takes only whole, aligned word writes, because byte
 * enables would keep a dual-port RAM from being inferred. */
#define VID_FONT16 ((volatile uint32_t *)0x50040000u)
#define VID_FONT8 ((volatile uint32_t *)0x50041000u)
#define VID_ITALIC16 ((volatile uint32_t *)0x50042000u)
#define VID_FONT_DEC16 ((volatile uint32_t *)0x50043000u)
#define VID_FONT_DEC8 ((volatile uint32_t *)0x50043200u)
#define VID_CANVAS (*(volatile uint32_t *)0x50028000u)
#define VID_VSYNC_LINE (*(volatile uint32_t *)0x50028004u)
#define REGS_WIN ((volatile uint8_t *)0x20000000u)
#define UART_POP (*(volatile uint32_t *)0x20000040u)
/* The low byte is the $FFF0 enable mask and the next byte holds the pending
 * sources. A write to the $FFF0 cell in REGS_WIN changes neither, because that
 * cell is a copy of the pending byte and is rewritten every clock. */
#define REGS_IRQ (*(volatile uint32_t *)0x20000044u)
#define RX_OFFER (*(volatile uint32_t *)0x20000048u)
#define AUD_PSG_XADDR (*(volatile uint32_t *)0x70000000u)
/* The PSG acts on the gate bit in a write to a channel block in XRAM only
 * when the 6502 made the write, unless this is set. aud_restore sets it
 * while it replays the channel blocks and clears it after, so a voice
 * whose gate bit was set when the blob was made starts again. */
#define AUD_PSG_REPLAY (*(volatile uint32_t *)0x70000004u)
#define AUD_OPL_XADDR (*(volatile uint32_t *)0x70000008u)
#define AUD_BEL_LO (*(volatile uint32_t *)0x70000010u)
#define AUD_BEL_HI (*(volatile uint32_t *)0x70000014u)
#define CPU_RESB (*(volatile uint8_t *)0x40000000u)
#define API_PENDING (*(volatile uint8_t *)0x40000004u)

#endif /* _HOST_POCKET_SW_MMIO_H_ */
