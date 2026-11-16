/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_MMIO_H_
#define _HOST_POCKET_SW_MMIO_H_

#include <stdint.h>

/* The bitstream's register layout version, which gen/version.py states.
 * ver_check reads it before the firmware uses any other register and
 * goes no further when it is not this firmware's own. So this address
 * never moves, whatever else in this file does. */
#define VER_REG (*(volatile const uint32_t *)0x80030000u)

#define MMIO_CONSOLE (*(volatile uint32_t *)0xF0000000u)
#define MMIO_PHI2 (*(volatile uint32_t *)0xF000000Cu)
/* mtime in microseconds. A write of MTIME_LO is kept until a write of
 * MTIME_HI puts the two in together, so the low half is written first. */
#define MTIME_LO (*(volatile uint32_t *)0xF0000010u)
#define MTIME_HI (*(volatile uint32_t *)0xF0000014u)
#define MMIO_SLOT (*(volatile uint32_t *)0xF0000018u)
/* Microseconds from configuration. Nothing stops or writes it, so it goes
 * on through a hold, when mtime stands still. */
#define UPTIME_LO (*(volatile const uint32_t *)0xF0000020u)
#define UPTIME_HI (*(volatile const uint32_t *)0xF0000024u)
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
/* The SDRAM at its bridge addresses, past the cache. It reads and
 * writes as memory does, a byte, a half or a word. The hardware keeps
 * the last 16-byte line it fetched, so a word read costs what a byte
 * read does, and a write drops that line. The cache holds only the
 * memory this firmware runs in, so everything else in the SDRAM is
 * reached here. */
#define STAGE ((volatile const uint8_t *)0x60000000u)

/* These addresses must agree with the slots in data.json.
 * stage_map_gate.py checks every one of them against it and against the
 * hardware. ROM_MAX is the ROM slot's size_maximum. Everything else is
 * packed above it in this order. The soft CPU's memory is at MEM_BRIDGE
 * and the soft CPU sees it at address 0: the first MEM_ROM_MAX bytes are
 * the ROM block, which the host loads as firmware.bin, and the
 * MEM_RAM_MAX bytes at MEM_RAM are the RAM block. MEM_MAX is the cache's
 * window, a power of two, and the soft CPU's alone: the two blocks can grow
 * in it. The eight file windows follow it, then the Get File scratch.
 * The savestate window is last, SST_BLOB_MAX bytes at SST_BLOB_BRIDGE
 * that run to the end of the SDRAM, where the host reads a save and
 * writes a load, both at the window's start. SST_BLOB is the window in the staging store, where the
 * firmware writes a save and reads a load. */
#define ROM_IMG ((volatile const uint8_t *)0x60000000u)
#define ROM_BRIDGE 0x00000000u
#define ROM_MAX 0x03E00000u
#define MEM_BRIDGE 0x03E00000u
#define MEM_MAX 0x00080000u
#define MEM_ROM_MAX 0x00040000u
#define MEM_RAM 0x00040000u
#define MEM_RAM_MAX 0x00010000u
#define SLOT_WIN_SIZE 0x8000u
#define SLOT_WIN_BASE ((volatile const uint8_t *)0x63E80000u)
#define SLOT_WIN(d) (SLOT_WIN_BASE + (uint32_t)(d) * SLOT_WIN_SIZE)
#define SLOT_WIN_BRIDGE(d) (0x03E80000u + (uint32_t)(d) * SLOT_WIN_SIZE)
#define FILE_XFER_MAX SLOT_WIN_SIZE
#define GETFILE_WIN ((volatile const uint8_t *)0x63EC0000u)
#define GETFILE_BRIDGE 0x03EC0000u
#define GETFILE_MAX 0x1000u
#define SST_BLOB_BRIDGE 0x03EC1000u
#define SST_BLOB ((uint8_t *)0x63EC1000u)
#define SST_BLOB_MAX 0x0013F000u

/* FILE_WIN is the write port of a dual-port RAM in pocket_file.sv. The
 * RAM's read port is on the bridge, so the firmware cannot read FILE_WIN
 * back, and the write port has no byte enables, so each write stores a
 * whole word. FILE_WIN_BASE is the bridge address of the same RAM and
 * must match WINDOW_BASE in pocket_file.sv.
 *
 * A write to FILE_CTL starts the command that the other registers
 * describe. */
#define FILE_ID (*(volatile uint32_t *)0x80000000u)
#define FILE_OFFSET (*(volatile uint32_t *)0x80000004u)
#define FILE_LENGTH (*(volatile uint32_t *)0x80000008u)
#define FILE_BRIDGE (*(volatile uint32_t *)0x8000000Cu)
#define FILE_CTL (*(volatile uint32_t *)0x80000010u)
#define FILE_RESULT (*(volatile uint32_t *)0x80000014u)
#define FILE_WIN ((volatile uint32_t *)0x80001000u)

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
#define SST_CTL (*(volatile uint32_t *)0x80020000u)
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
#define SET_KB (*(volatile uint32_t *)0x80010000u)
#define SET_TZ_HOUR (*(volatile uint32_t *)0x80010008u)
#define RTC_EPOCH (*(volatile uint32_t *)0x8001000Cu)
#define RTC_VALID (*(volatile uint32_t *)0x80010010u)
#define SET_TZ_MIN (*(volatile uint32_t *)0x80010014u)
#define SET_TZ_WEST (*(volatile uint32_t *)0x80010018u)
/* 1 from the host's request to write the ROM slot until the new image's
 * size is in MMIO_SLOT. The bitstream holds the 6502 in reset meanwhile. */
#define ROM_COMING (*(volatile const uint32_t *)0x8001001Cu)

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

/* mode0.sv's registers from 0x50010000 are core/vga/mode/mode0.h's
 * mode0_regs, which link.ld places there. */
/* The frames since power-up. A write sets the count. */
#define VID_FRAME (*(volatile uint32_t *)0x500100A0u)

/* The scanline program, which reads back as it was written, less the
 * bits it does not keep: core/vga/vga.h's VGA_PROG_MAX lines of
 * SCANVIDEO_PLANE_COUNT planes, VID_XPROG_WORDS words a plane, at a
 * stride of 16 words a line. */
#define VID_XPROG_WORDS 4
#define VID_XPROG(line, plane, w) \
    (((volatile uint32_t *)0x50020000u)[(line) * 16 + (plane) * 4 + (w)])
#define VID_CANVAS (*(volatile uint32_t *)0x50028000u)
#define VID_VSYNC_LINE (*(volatile uint32_t *)0x50028004u)
#define REGS_WIN ((volatile uint8_t *)0x20000000u)
#define UART_POP (*(volatile uint32_t *)0x20000040u)
/* The low byte is the $FFF0 enable mask and the next byte holds the pending
 * sources. A write to the $FFF0 cell in REGS_WIN changes neither, because that
 * cell is a copy of the pending byte and is rewritten every clock. */
#define REGS_IRQ (*(volatile uint32_t *)0x20000044u)
/* A write sets SIGINT, bit 6 of the pending sources. A read returns what
 * REGS_IRQ does and clears SIGINT on the same clock, so RIA_ATTR_SIGINT and
 * the $FFF0 interrupt share one latch. */
#define REGS_SIGINT (*(volatile uint32_t *)0x2000006Cu)
#define RX_OFFER (*(volatile uint32_t *)0x20000048u)
/* Bit 8 is set while the 6502 has a byte from the OS it has not read, in the
 * offer slot or in its receive register, and bits 7:0 are that byte. A read
 * of RX_RECLAIM also takes the byte back, unless a $FFE2 read on the same
 * clock takes it first. */
#define RX_PEEK (*(volatile uint32_t *)0x20000064u)
#define RX_RECLAIM (*(volatile uint32_t *)0x20000068u)
#define AUD_PSG_XADDR (*(volatile uint32_t *)0x70000000u)
/* 1 from a write to AUD_PSG_XADDR until the PSG has zeroed its eight
 * voices' registers, about 64 clocks after any sample in progress ends. */
#define AUD_PSG_BUSY (*(volatile uint32_t *)0x70000004u)
#define AUD_OPL_XADDR (*(volatile uint32_t *)0x70000008u)
/* The OPL's overruns, writes dropped because its queue was full, since the
 * last read, which clears it. */
#define AUD_OPL_OVERRUNS (*(volatile uint32_t *)0x7000000Cu)
/* One byte of the bell's channel structure: bits 10:8 its index, 7:0 its
 * value. Byte 6, the gate, strikes or releases. */
#define AUD_BEL (*(volatile uint32_t *)0x70000010u)
/* The PCM: device. Bit 0 opens or closes, bit 1 is 16-bit and bit 2 is
 * stereo. An open empties the buffer; the sound restarts at the next
 * sample. */
#define AUD_PCM_CTRL (*(volatile uint32_t *)0x70000018u)
/* Frames per 48 kHz sample, Q32, less one: pcm_step(rate) - 1. Written
 * while closed. */
#define AUD_PCM_STEP (*(volatile uint32_t *)0x7000001Cu)
/* Four bytes into the buffer, the low byte first, at a word of it. */
#define AUD_PCM_DATA (*(volatile uint32_t *)0x70000020u)
/* One byte into the buffer, in every lane of the word. */
#define AUD_PCM_BYTE (*(volatile uint32_t *)0x70000024u)
/* Empties the buffer. */
#define AUD_PCM_CLEAR (*(volatile uint32_t *)0x70000028u)
/* The bytes waiting to play, 0 to 4096. Nothing checks a write against
 * the room, 4096 less this. */
#define AUD_PCM_LEVEL (*(volatile uint32_t *)0x7000002Cu)
/* Times the buffer ran dry since the last read or open. A read clears
 * it. */
#define AUD_PCM_UNDERRUNS (*(volatile uint32_t *)0x70000030u)
/* Makes the bytes written since the last commit playable at once, as one
 * write. */
#define AUD_PCM_COMMIT (*(volatile uint32_t *)0x70000034u)
#define CPU_RESB (*(volatile uint8_t *)0x40000000u)
#define API_PENDING (*(volatile uint8_t *)0x40000004u)

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
#define MACH_HOLD (*(volatile uint32_t *)0x90000000u)
#define MACH_HOLD_ON 0x01u
#define MACH_HELD 0x02u
#define ST_SEL (*(volatile uint32_t *)0x90000004u)
#define ST_DATA (*(volatile const uint32_t *)0x90000008u)
#define ST_PICK(group, word) (((group) << 3) | (word))
#define ST_MACH 0u
#define ST_CPU 1u
#define ST_VIA 2u
#define ST_BUS 3u
#define ST_MACH_WORDS 2
#define ST_CPU_WORDS 5
#define ST_VIA_WORDS 7
#define ST_BUS_WORDS 1
#define JAM (*(volatile uint32_t *)0x9000000Cu)
#define REGS_RESTORE (*(volatile uint32_t *)0x90000010u)
#define AUD_OWN (*(volatile uint32_t *)0x90000014u)
#define JAM_DATA(n) (((volatile uint32_t *)0x90000040u)[n])
#define JAM_CPU 0
#define JAM_VIA 5
#define JAM_MACH 12
#define JAM_BUS 14
#define AUD_SST(w) (((volatile uint32_t *)0x90002000u)[w])
#define AUD_SST_WORDS 2048

#endif /* _HOST_POCKET_SW_MMIO_H_ */
