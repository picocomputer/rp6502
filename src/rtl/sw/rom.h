/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_ROM_H_
#define _FPGA_SW_ROM_H_

#include "ria/api/api.h"
#include "ria/api/std.h"

#include <stdbool.h>
#include <stdint.h>

/* Load the .rp6502 image the platform staged, len bytes at ROM_IMG.
 * True when the program and its reset vector are in place. */
bool rom_load_staged(uint32_t len);

/* A window of the staging store, said at boot and again at a resume.
 * Whether the host puts the memory's own ROM back in slot 0 or leaves
 * whatever this boot loaded is a question only the device answers, and
 * two readings of the same bytes answer it. Nothing here writes. */
#ifdef RP6502_LOG_FILE
void rom_probe(const char *tag);
#else
static inline void rom_probe(const char *tag) { (void)tag; }
#endif

/* The ROM: drive: read-only windows onto the staged image's assets,
 * registered in main_std_drivers. */
bool rom_std_handles(const char *path);
int rom_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result rom_std_close(int desc, api_errno *err);
std_rw_result rom_std_read(int desc, char *buf, uint32_t count,
                           uint32_t *got, api_errno *err);
int rom_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos,
                  api_errno *err);

#endif /* _FPGA_SW_ROM_H_ */
