/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The Pocket's side of the soft CPU's map: pocket_core's host window and
 * the staging layout of its SDRAM. The machine's map is
 * host/fpga/sw/mmio.h. */

#ifndef _HOST_POCKET_SW_BRIDGE_H_
#define _HOST_POCKET_SW_BRIDGE_H_

#include <stdint.h>

/* These addresses must agree with the slots in data.json.
 * stage_map_gate.py checks every one of them against it and against the
 * hardware. The bottom 2 MB of the SDRAM holds, in this order: the soft
 * CPU's memory, mmio.h's MEM_MAX bytes at MEM_BRIDGE, which is 0 so the
 * soft CPU's address is the SDRAM's; the eight file windows; the Get File
 * scratch; and last the savestate window, SST_BLOB_MAX bytes at
 * SST_BLOB_BRIDGE, where the host reads a save and writes a load, both at
 * the window's start. The ROM slot is everything above, ROM_MAX bytes at
 * ROM_BRIDGE to the end of the SDRAM. SST_BLOB is the window in the
 * staging store, where the firmware writes a save and reads a load. */
#define ROM_IMG ((volatile const uint8_t *)(uintptr_t)0x60200000u)
#define ROM_BRIDGE 0x00200000u
#define ROM_MAX 0x03E00000u
#define MEM_BRIDGE 0x00000000u
#define SLOT_WIN_SIZE 0x8000u
#define SLOT_WIN_BASE ((volatile const uint8_t *)(uintptr_t)0x60080000u)
#define SLOT_WIN(d) (SLOT_WIN_BASE + (uint32_t)(d) * SLOT_WIN_SIZE)
#define SLOT_WIN_BRIDGE(d) (0x00080000u + (uint32_t)(d) * SLOT_WIN_SIZE)
#define FILE_XFER_MAX SLOT_WIN_SIZE
#define GETFILE_WIN ((volatile const uint8_t *)(uintptr_t)0x600C0000u)
#define GETFILE_BRIDGE 0x000C0000u
#define GETFILE_MAX 0x1000u
#define SST_BLOB_BRIDGE 0x000C1000u
#define SST_BLOB ((uint8_t *)0x600C1000u)
#define SST_BLOB_MAX 0x0013F000u

/* FILE_WIN is the write port of a dual-port RAM in pocket_file.sv. The
 * RAM's read port is on the bridge, so the firmware cannot read FILE_WIN
 * back, and the write port has no byte enables, so each write stores a
 * whole word. FILE_WIN_BASE is the bridge address of the same RAM and
 * must match WINDOW_BASE in pocket_file.sv.
 *
 * A write to FILE_CTL starts the command that the other registers
 * describe. */
#define FILE_ID (*(volatile uint32_t *)(uintptr_t)0x80000000u)
#define FILE_OFFSET (*(volatile uint32_t *)(uintptr_t)0x80000004u)
#define FILE_LENGTH (*(volatile uint32_t *)(uintptr_t)0x80000008u)
#define FILE_BRIDGE (*(volatile uint32_t *)(uintptr_t)0x8000000Cu)
#define FILE_CTL (*(volatile uint32_t *)(uintptr_t)0x80000010u)
#define FILE_RESULT (*(volatile uint32_t *)(uintptr_t)0x80000014u)
#define FILE_WIN ((volatile uint32_t *)(uintptr_t)0x80001000u)

/* The host's savestate commands. SST_SAVE_REQ reads set from the host's
 * save request until the firmware writes SST_SAVE_OK or SST_SAVE_ERR,
 * which the host is then answered with. The host reads POCKET_SST_BYTES
 * of the window once it has its ok. SST_LOAD_REQ reads set from the
 * host's load request until the firmware writes SST_LOAD_OK or
 * SST_LOAD_ERR. A load the host asked for while it held the core in
 * reset was answered ok at once, and its request is here when the
 * firmware boots. SST_BLOB_SEEN is set by a host write into the window
 * or by a load request, either of which can come first, and clears when
 * the load is answered: while it is set a load is on its way.
 * SST_WQ_EMPTY is set while no host write waits to reach the SDRAM, so
 * a blob the host wrote is all in the window. */
#define SST_CTL (*(volatile uint32_t *)(uintptr_t)0x80020000u)
#define SST_SAVE_REQ 0x01u
#define SST_LOAD_REQ 0x02u
#define SST_BLOB_SEEN 0x04u
#define SST_WQ_EMPTY 0x08u
#define SST_SAVE_OK 1u
#define SST_SAVE_ERR 2u
#define SST_LOAD_OK 3u
#define SST_LOAD_ERR 4u
/* The UTC offset comes from three interact menu entries because a menu
 * list holds at most sixteen options and the offset spans twenty-seven
 * whole hours. SET_KB is a position in def/keyboard.def plus one, so zero
 * means that the host has not written a layout to it. */
#define SET_KB (*(volatile uint32_t *)(uintptr_t)0x80010000u)
#define SET_TZ_HOUR (*(volatile uint32_t *)(uintptr_t)0x80010008u)
#define RTC_EPOCH (*(volatile uint32_t *)(uintptr_t)0x8001000Cu)
#define RTC_VALID (*(volatile uint32_t *)(uintptr_t)0x80010010u)
#define SET_TZ_MIN (*(volatile uint32_t *)(uintptr_t)0x80010014u)
#define SET_TZ_WEST (*(volatile uint32_t *)(uintptr_t)0x80010018u)
/* The controllers' registers, APF's key, joystick and trigger words for
 * each of its slots. */
#define MMIO_CONT_KEY(s) (*(volatile const uint32_t *)(uintptr_t)(0x80010040u + (s) * 12u))
#define MMIO_CONT_JOY(s) (*(volatile const uint32_t *)(uintptr_t)(0x80010044u + (s) * 12u))
#define MMIO_CONT_TRIG(s) (*(volatile const uint32_t *)(uintptr_t)(0x80010048u + (s) * 12u))
#define MMIO_CONT_SLOTS 4

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
/* core_bridge_cmd ends a data slot command with result 7 when the host
 * has not finished it within about 0.9 s, half of pocket_file's deadline,
 * so FILE_ST_TIMEOUT after a write to FILE_CTL means that core_bridge_cmd
 * itself has stopped. FILE_ST_TIMEOUT also reads set from the time the FPGA
 * is configured until the first write to FILE_CTL. */

#define FILE_ST_BUSY 0x01u
#define FILE_ST_ERR 0x0Eu
#define FILE_ST_TIMEOUT 0x10u
/* A slot operation can complete while the write FIFO in
 * pocket_bridge.sv still holds writes for the staging store, so a read of
 * the new data waits for this bit to clear. */
#define FILE_ST_DRAIN 0x20u
#define FILE_ST_WROTE 0x40u

#endif /* _HOST_POCKET_SW_BRIDGE_H_ */
