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

bool rom_load_staged(uint32_t len);

void rom_log(void);

bool rom_std_handles(const char *path);
int rom_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result rom_std_close(int desc, api_errno *err);
std_rw_result rom_std_read(int desc, char *buf, uint32_t count,
                           uint32_t *got, api_errno *err);
int rom_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos,
                  api_errno *err);

#endif
