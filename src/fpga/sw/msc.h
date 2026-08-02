/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_MSC_H_
#define _FPGA_SW_MSC_H_

#include "ria/api/api.h"
#include "ria/api/std.h"

#include <stdbool.h>
#include <stdint.h>

/* The host's filesystem, over APF data slots. The catch-all std driver,
 * so it must be last in main_std_drivers: it claims every path no
 * earlier driver took, which is what the shared std.c expects of the
 * mass-storage drive on the real machine. */
/* Publish the nonvolatile slot's size into the data table, once at
 * boot, so the host persists it — and with it the drive's folder — at
 * the first Quit, power-off or sleep. */
void msc_init(void);

/* The pinned working directory: getcwd answers the host's own, which
 * is MSC0:/Saves/rp6502/common/ and 26 characters; chdir always
 * errors, chdrive accepts only this drive's names. */
bool msc_api_getcwd(void);
bool msc_api_chdir(void);
bool msc_api_chdrive(void);

bool msc_std_handles(const char *path);
int msc_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result msc_std_close(int desc, api_errno *err);
std_rw_result msc_std_read(int desc, char *buf, uint32_t count,
                           uint32_t *got, api_errno *err);
std_rw_result msc_std_write(int desc, const char *buf, uint32_t count,
                            uint32_t *wrote, api_errno *err);
std_rw_result msc_std_sync(int desc, api_errno *err);
int msc_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos,
                  api_errno *err);

#endif /* _FPGA_SW_MSC_H_ */
