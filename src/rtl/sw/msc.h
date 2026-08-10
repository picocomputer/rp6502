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
#include <stddef.h>
#include <stdint.h>

/* The host's filesystem, over APF data slots. The catch-all std driver,
 * so it must be last in main_std_drivers. */

/* No working directory on this platform, so each side of the API pins
 * its own folder: std open resolves relative paths under SAVES, a
 * program name under ASSETS. An absolute path travels untouched. */
#define MSC_SAVES_PATH "/Saves/rp6502/common/"
#define MSC_ASSETS_PATH "/Assets/rp6502/common/"

/* First in data.json, because a hot reload writes the new image through
 * the first slot record. */
#define MSC_SLOT_ROM 0

/* Lands whatever a worker left at the bridge, so the next command does
 * not stack on top of it. */
void msc_stop(void);

/* The open files opened again, because the host forgets which path
 * each data slot was for when the core is reconfigured. */
void msc_restore(void);

/* By word index, not by slot: the table is id/size pairs and the host
 * decides where each pair lands. */
uint32_t msc_dt(uint32_t word);

/* Blocking. */
bool msc_slot_len(uint32_t slot, uint32_t *len);

/* Converted to code page bytes, refused rather than truncated if it will
 * not fit. Blocking, so staging time only. */
bool msc_getfile(uint32_t slot, char *out, size_t cap);

/* Pull a whole .rp6502 into the staging store where the host puts one,
 * so the loader parses it the same way either arrived. Blocking, and
 * only ever called with the 6502 stopped. */
bool msc_stage_rom(const char *path, uint32_t *len);

/* chdir always errors; chdrive accepts only this drive's names. */
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
