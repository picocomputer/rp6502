/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_SYS_BUF1K_H_
#define _CORE_SYS_BUF1K_H_

#include <stdint.h>

/* One kilobyte of scratch memory, on a word, for work that is done before
 * anything else can run or that only one thing at a time can ask for. It
 * holds nothing between uses, no savestate carries it, and nothing guards
 * it: a new user must be one that cannot overlap any user below.
 *
 * Its users, on every machine with a ROM to load:
 *
 * - The ROM reader, core/rom/pump.c: the header lines and each record of a
 *   .rp6502 as it is read. A record's data is in here until the next read.
 *
 * And on the RP6502-RIA, where the monitor has one command at a time and
 * the 6502 is stopped for it:
 *
 * - ria/sys/mbuf.c: bytes read from the console, counted in mbuf_len.
 * - ria/sys/ria.c: what a transfer to or from 6502 memory moves.
 * - ria/sys/cfg.c: a line of the settings file.
 * - ria/sys/vga.c: the VGA's answer on the backchannel.
 * - ria/mon/ram.c: memory being shown, a line of hex, an upload's chunk.
 * - ria/mon/rom.c: the ROM reader's, an install's copy, a ROM's help text,
 *   and the boot setting's string.
 * - ria/mon/fil.c: a path, an upload's chunk, a copy's block.
 * - ria/mon/drive.c: sectors, and the work area of a format.
 * - ria/mon/uf2.c and uf2_write.c: a UF2 block and the flash page built
 *   from it.
 * - ria-w/net/wifi.c: the access points of a scan.
 * - ria-w/net/cyw.c: the two buffers the radio's driver loads the
 *   Bluetooth firmware through, at boot and after a reset of the radio,
 *   which waits for every command above to be done.
 *
 * It is not the place for anything kept, or for work in a task that runs
 * beside the monitor or a running program. */
#define BUF1K_SIZE 1024
extern uint8_t buf1k[BUF1K_SIZE];

#endif /* _CORE_SYS_BUF1K_H_ */
