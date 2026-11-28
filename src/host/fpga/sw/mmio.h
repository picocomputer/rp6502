/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The machine's map, as the soft CPU sees it: what every FPGA host's
 * bitstream decodes alike. A host's own registers and its staging layout
 * are in its sw/bridge.h. */

#ifndef _HOST_FPGA_SW_MMIO_H_
#define _HOST_FPGA_SW_MMIO_H_

#include <stdint.h>

/* Each address goes to a pointer through uintptr_t, because the host's tests
 * build these sources too, where a pointer is wider than the soft CPU's 32
 * bits. */

/* The bitstream's register layout version, which the host's
 * gen/version.py composes with src/host/fpga/gen/version.py. ver_check
 * reads it before the firmware uses any other register and goes no
 * further when it is not this firmware's own. So this address never
 * moves, whatever else in this file does. */
#define VER_REG (*(volatile const uint32_t *)(uintptr_t)0x80030000u)

#define MMIO_CONSOLE (*(volatile uint32_t *)(uintptr_t)0xF0000000u)
#define MMIO_PHI2 (*(volatile uint32_t *)(uintptr_t)0xF000000Cu)
/* mtime in microseconds. A write of MTIME_LO is kept until a write of
 * MTIME_HI puts the two in together, so the low half is written first. */
#define MTIME_LO (*(volatile uint32_t *)(uintptr_t)0xF0000010u)
#define MTIME_HI (*(volatile uint32_t *)(uintptr_t)0xF0000014u)
/* The host's ROM landing. MMIO_ROM_LEN is the size of the image the host
 * has staged, loaded with each landing and cleared by the firmware once it
 * has handled it. MMIO_ROM_STOP is 1 from the host's request to write a
 * new ROM until its size is in MMIO_ROM_LEN, and the bitstream holds the
 * 6502 in reset meanwhile. MMIO_ROM_UPD counts the host's ROM loads. */
#define MMIO_ROM_LEN (*(volatile uint32_t *)(uintptr_t)0xF0000018u)
#define MMIO_ROM_STOP (*(volatile const uint32_t *)(uintptr_t)0xF000001Cu)
/* Microseconds from configuration. Nothing stops or writes it, so it goes
 * on through a hold, when mtime stands still. */
#define UPTIME_LO (*(volatile const uint32_t *)(uintptr_t)0xF0000020u)
#define UPTIME_HI (*(volatile const uint32_t *)(uintptr_t)0xF0000024u)
#define MMIO_ROM_UPD (*(volatile const uint32_t *)(uintptr_t)0xF000004Cu)

#define SRAM ((volatile uint8_t *)(uintptr_t)0x10000000u)
#define XRAM_WIN ((volatile uint8_t *)(uintptr_t)0x30000000u)
/* The staging store, the host's memory (the Pocket's SDRAM) at its
 * bridge addresses, past the cache. It reads and writes as memory does,
 * a byte, a half or a word. The hardware keeps the last 16-byte line it
 * fetched, so a word read costs what a byte read does, and a write drops
 * that line. The cache holds only the memory this firmware runs in, so
 * everything else in the store is reached here. */
#define STAGE ((volatile const uint8_t *)(uintptr_t)0x60000000u)

/* The soft CPU's memory, which it sees at address 0 and every host keeps
 * at the start of its staging store, so STAGE + n is the soft CPU's n:
 * the first MEM_ROM_MAX bytes are the ROM block,
 * which the host loads as firmware.bin, and the MEM_RAM_MAX bytes at
 * MEM_RAM are the RAM block. MEM_MAX is the cache's window, a power of
 * two, and the soft CPU's alone: the two blocks can grow in it. The
 * Pocket's stage_map_gate.py checks these against link.ld and the
 * hardware. */
#define MEM_MAX 0x00080000u
#define MEM_ROM_MAX 0x00040000u
#define MEM_RAM 0x00040000u
#define MEM_RAM_MAX 0x00010000u

/* mode0.sv's registers from 0x50010000 are core/vga/mode/mode0.h's
 * mode0_regs, which link.ld places there, and above them the frame count. */
/* The frames since power-up. A write sets the count. */
#define VID_FRAME (*(volatile uint32_t *)(uintptr_t)0x500100FCu)

/* The scanline program, which reads back as it was written, less the
 * bits it does not keep: core/vga/vga.h's VGA_PROG_MAX lines of
 * SCANVIDEO_PLANE_COUNT planes, VID_XPROG_WORDS words a plane, at a
 * stride of 16 words a line. */
#define VID_XPROG_WORDS 4
#define VID_XPROG(line, plane, w) \
    (((volatile uint32_t *)(uintptr_t)0x50020000u)[(line) * 16 + (plane) * 4 + (w)])
#define VID_CANVAS (*(volatile uint32_t *)(uintptr_t)0x50028000u)
#define VID_VSYNC_LINE (*(volatile uint32_t *)(uintptr_t)0x50028004u)

/* mode6.sv's pointer, which takes whole words and reads back nothing: the
 * shape, 0 for none, with the copy of the system shape's image to draw
 * in bit 8; the hot spot {y, x} in canvas pixels; the program's sprite
 * array in XRAM, or $FFFF for the system's shape; each copy's
 * {width_height, hot_y, hot_x}; and each copy's image, 128 words. The
 * shape, the hot spot and the config latch as a frame starts. */
#define VID_PTR_SHAPE (*(volatile uint32_t *)(uintptr_t)0x50200000u)
#define VID_PTR_WHERE (*(volatile uint32_t *)(uintptr_t)0x50200004u)
#define VID_PTR_CONFIG (*(volatile uint32_t *)(uintptr_t)0x50200008u)
#define VID_PTR_SYS(copy) (((volatile uint32_t *)(uintptr_t)0x5020000Cu)[copy])
#define VID_PTR_IMG_WORDS 128
#define VID_PTR_IMG(copy, w) \
    (((volatile uint32_t *)(uintptr_t)0x50201000u)[(copy) * VID_PTR_IMG_WORDS + (w)])
#define REGS_WIN ((volatile uint8_t *)(uintptr_t)0x20000000u)
#define UART_POP (*(volatile uint32_t *)(uintptr_t)0x20000040u)
/* The low byte is the $FFF0 enable mask and the next byte holds the pending
 * sources. A write to the $FFF0 cell in REGS_WIN changes neither, because that
 * cell is a copy of the pending byte and is rewritten every clock. */
#define REGS_IRQ (*(volatile uint32_t *)(uintptr_t)0x20000044u)
/* A write sets SIGINT, bit 6 of the pending sources. A read returns what
 * REGS_IRQ does and clears SIGINT on the same clock, so RIA_ATTR_SIGINT and
 * the $FFF0 interrupt share one latch. */
#define REGS_SIGINT (*(volatile uint32_t *)(uintptr_t)0x2000006Cu)
#define RX_OFFER (*(volatile uint32_t *)(uintptr_t)0x20000048u)
/* Bit 8 is set while the 6502 has a byte from the OS it has not read, in the
 * offer slot or in its receive register, and bits 7:0 are that byte. A read
 * of RX_RECLAIM also takes the byte back, unless a $FFE2 read on the same
 * clock takes it first. */
#define RX_PEEK (*(volatile uint32_t *)(uintptr_t)0x20000064u)
#define RX_RECLAIM (*(volatile uint32_t *)(uintptr_t)0x20000068u)
#define AUD_PSG_XADDR (*(volatile uint32_t *)(uintptr_t)0x70000000u)
/* 1 from a write to AUD_PSG_XADDR until the PSG has zeroed its eight
 * voices' registers, about 64 clocks after any sample in progress ends. */
#define AUD_PSG_BUSY (*(volatile uint32_t *)(uintptr_t)0x70000004u)
#define AUD_OPL_XADDR (*(volatile uint32_t *)(uintptr_t)0x70000008u)
/* The OPL's overruns, writes dropped because its queue was full, since the
 * last read, which clears it. */
#define AUD_OPL_OVERRUNS (*(volatile uint32_t *)(uintptr_t)0x7000000Cu)
/* One byte of the bell's channel structure: bits 10:8 its index, 7:0 its
 * value. Byte 6, the gate, strikes or releases. */
#define AUD_BEL (*(volatile uint32_t *)(uintptr_t)0x70000010u)
/* The PCM: device. Bit 0 opens or closes, bit 1 is 16-bit and bit 2 is
 * stereo. An open empties the buffer; the sound restarts at the next
 * sample. */
#define AUD_PCM_CTRL (*(volatile uint32_t *)(uintptr_t)0x70000018u)
/* Frames per 48 kHz sample, Q32, less one: pcm_step(rate) - 1. Written
 * while closed. */
#define AUD_PCM_STEP (*(volatile uint32_t *)(uintptr_t)0x7000001Cu)
/* Four bytes into the buffer, the low byte first, at a word of it. */
#define AUD_PCM_DATA (*(volatile uint32_t *)(uintptr_t)0x70000020u)
/* One byte into the buffer, in every lane of the word. */
#define AUD_PCM_BYTE (*(volatile uint32_t *)(uintptr_t)0x70000024u)
/* Empties the buffer. */
#define AUD_PCM_CLEAR (*(volatile uint32_t *)(uintptr_t)0x70000028u)
/* The bytes waiting to play, 0 to 4096. Nothing checks a write against
 * the room, 4096 less this. */
#define AUD_PCM_LEVEL (*(volatile uint32_t *)(uintptr_t)0x7000002Cu)
/* Times the buffer ran dry since the last read or open. A read clears
 * it. */
#define AUD_PCM_UNDERRUNS (*(volatile uint32_t *)(uintptr_t)0x70000030u)
/* Makes the bytes written since the last commit playable at once, as one
 * write. */
#define AUD_PCM_COMMIT (*(volatile uint32_t *)(uintptr_t)0x70000034u)
#define CPU_RESB (*(volatile uint8_t *)(uintptr_t)0x40000000u)
#define API_PENDING (*(volatile uint8_t *)(uintptr_t)0x40000004u)

/* The machine's state for a savestate. MACH_HOLD written with
 * MACH_HOLD_ON stops the machine on the first clock its audio engines
 * are quiet: the 6502, the VIA and the bus stand still, and so do the
 * audio engines' sample counters and the RIA's vsync. MACH_HELD reads
 * set from then until MACH_HOLD is written 0, which lets everything go
 * on the same clock. Everything below is read and written with the
 * machine held.
 *
 * ST_SEL picks a word of a group with ST_PICK and ST_DATA reads it.
 * ST_MACH's word 0 is the pending PHI2 enable in bit 1 and RESB in bit
 * 0, and its word 1 the PHI2 accumulator in bits 31:16 and the rate in
 * bits 15:0. ST_BUS's word is the open-bus byte.
 *
 * JAM_DATA holds the words of all four groups at JAM_CPU, JAM_VIA,
 * JAM_MACH and JAM_BUS, laid out as ST_DATA reads them, and a write to
 * JAM loads them all at once. Only the pending PHI2 enable, the PHI2
 * accumulator and the open-bus byte are taken from ST_MACH's and ST_BUS's
 * words. The 6502 and the VIA take theirs only out of reset, so
 * CPU_RESB is written first.
 *
 * REGS_RESTORE set has writes to REGS_WIN put back what reads of it
 * gave: word 18 the RX state and the API flag, word 20 the console
 * queue's pointers, and word 200 the xstack pointer with its $FFEC
 * mirror. Words 16, 26 and 27 have side effects when read, and a
 * savestate leaves them out.
 *
 * AUD_OWN set gives the audio engines' state to AUD_SST, at psg.sv's,
 * opl.sv's and pcm.sv's savestate addresses. It is taken only while
 * MACH_HELD reads set, and the hold's end drops it. */
#define MACH_HOLD (*(volatile uint32_t *)(uintptr_t)0x90000000u)
#define MACH_HOLD_ON 0x01u
#define MACH_HELD 0x02u
#define ST_SEL (*(volatile uint32_t *)(uintptr_t)0x90000004u)
#define ST_DATA (*(volatile const uint32_t *)(uintptr_t)0x90000008u)
#define ST_PICK(group, word) (((group) << 3) | (word))
#define ST_MACH 0u
#define ST_CPU 1u
#define ST_VIA 2u
#define ST_BUS 3u
#define ST_MACH_WORDS 2
#define ST_CPU_WORDS 5
#define ST_VIA_WORDS 7
#define ST_BUS_WORDS 1
#define JAM (*(volatile uint32_t *)(uintptr_t)0x9000000Cu)
#define REGS_RESTORE (*(volatile uint32_t *)(uintptr_t)0x90000010u)
#define AUD_OWN (*(volatile uint32_t *)(uintptr_t)0x90000014u)
#define JAM_DATA(n) (((volatile uint32_t *)(uintptr_t)0x90000040u)[n])
#define JAM_CPU 0
#define JAM_VIA 5
#define JAM_MACH 12
#define JAM_BUS 14
#define AUD_SST(w) (((volatile uint32_t *)(uintptr_t)0x90002000u)[w])
#define AUD_SST_WORDS 2048

#endif /* _HOST_FPGA_SW_MMIO_H_ */
