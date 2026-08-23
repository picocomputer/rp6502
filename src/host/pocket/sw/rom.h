/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_ROM_H_
#define _FPGA_SW_ROM_H_

#include "core/api/api.h"
#include "core/api/std.h"

#include <stdbool.h>
#include <stdint.h>

/* Load the .rp6502 image the platform staged, len bytes at ROM_IMG.
 * True when the program and its reset vector are in place. */
bool rom_load_staged(uint32_t len);

/* The length of the image the running session loaded. After a restore
 * that is the blob's answer, and the store is supposed to be holding
 * that many bytes of that program. */
uint32_t rom_staged_len(void);


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
