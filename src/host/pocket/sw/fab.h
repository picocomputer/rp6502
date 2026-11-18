/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_FAB_H_
#define _HOST_POCKET_SW_FAB_H_

#include "core/sys/sst.h"
#include "core/vga/pixel_format.h"
#include "core/vga/vga.h"
#include "mmio.h"

/* The machine the fabric holds and no driver keeps: mtime, the frame
 * count, the 6502, the VIA, the PHI2 count and the open bus, the RIA's
 * registers and xstack, the scanline table, the 6502's RAM and XRAM. The
 * row is saved and loaded with the machine held, under MACH_HOLD. The load
 * leaves the 6502 in or out of reset as it was saved, still held.
 *
 * The audio engines are the AUD row's, and the cells are the CELL row's.
 *
 * mtime 8, frame 4, the jam words, the regs words, the scanline table,
 * then 64 KB of RAM and 64 KB of XRAM. */
#define FAB_JAM_WORDS (JAM_BUS + ST_BUS_WORDS)

/* The RIA's words a savestate carries, as runs of REGS_WIN's word numbers,
 * X(first, count): the 32 registers, the interrupt word and the RX word,
 * the console queue's pointers and bytes, the xstack with its guard, and
 * the xstack pointer. Word 16 is left out, because a read of it pops the
 * console queue. The pointer is last, because REGS_RESTORE has its write
 * refill $FFEC from the xstack and the registers already put back. */
#define FAB_REGS_RUNS(X) X(0, 8) X(17, 2) X(20, 5) X(64, 129) X(200, 1)
#define FAB_REGS_COUNT(at, n) +(n)
#define FAB_REGS_WORDS (0 FAB_REGS_RUNS(FAB_REGS_COUNT))
#define FAB_XPROG_WORDS (VGA_PROG_MAX * SCANVIDEO_PLANE_COUNT * VID_XPROG_WORDS)
#define FAB_SST_SIZE (8 + 4 + 4 * (FAB_JAM_WORDS + FAB_REGS_WORDS \
                                   + FAB_XPROG_WORDS)             \
                      + 2 * 0x10000)
void fab_sst_save(sst_cursor_t *c, unsigned flags);
bool fab_sst_load(sst_cursor_t *c, unsigned flags);

#define FAB_DRIVER DRIVER(                                     \
    SST(FAB_, 1, FAB_SST_SIZE, fab_sst_save, fab_sst_load))

#endif /* _HOST_POCKET_SW_FAB_H_ */
