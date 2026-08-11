/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_MMIO_H_
#define _FPGA_SW_MMIO_H_

#include <stdint.h>

#define MMIO_CONSOLE (*(volatile uint32_t *)0xF0000000u)
#define MMIO_CONSOLE_FULL 0x01u
#define MMIO_KBD (*(volatile uint32_t *)0xF0000008u)
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

#define FILE_ID (*(volatile uint32_t *)0x80000000u)
#define FILE_OFFSET (*(volatile uint32_t *)0x80000004u)
#define FILE_LENGTH (*(volatile uint32_t *)0x80000008u)
#define FILE_BRIDGE (*(volatile uint32_t *)0x8000000Cu)
#define FILE_CTL (*(volatile uint32_t *)0x80000010u)
#define FILE_RESULT (*(volatile uint32_t *)0x80000014u)
#define FILE_WIN ((volatile uint32_t *)0x80001000u)

#define SST_CTL (*(volatile uint32_t *)0x80020000u)

#define SST_RESTORED 0x01u
#define SST_BLOB_SEEN 0x02u
#define SST_UNDERRUN 0x04u
#define SST_RESTORE_ERR 0x08u
#define SST_SAVED 0x10u
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
#define FILE_OP_FLUSH 6u

#define FILE_ST_BUSY 0x01u
#define FILE_ST_ERR 0x0Eu
#define FILE_ST_TIMEOUT 0x10u
#define FILE_ST_DRAIN 0x20u

#define VID_ROW(i) (((volatile uint32_t *)0x50010000u)[i])
#define VID_CURSOR (*(volatile uint32_t *)0x50010080u)
#define VID_CURSOR_COLOR (*(volatile uint32_t *)0x50010084u)
#define VID_BLINK (*(volatile uint32_t *)0x50010088u)
#define VID_PROG (*(volatile uint32_t *)0x5001008Cu)
#define VID_FRAME (*(volatile uint32_t *)0x500100A0u)

#define VID_XPROG(line, plane, w) \
    (((volatile uint32_t *)0x50020000u)[(line) * 16 + (plane) * 4 + (w)])
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
#define AUD_PSG_REPLAY (*(volatile uint32_t *)0x70000004u)
#define AUD_OPL_XADDR (*(volatile uint32_t *)0x70000008u)
#define AUD_BEL_LO (*(volatile uint32_t *)0x70000010u)
#define AUD_BEL_HI (*(volatile uint32_t *)0x70000014u)
#define CPU_RESB (*(volatile uint8_t *)0x40000000u)
#define API_PENDING (*(volatile uint8_t *)0x40000004u)

#endif
