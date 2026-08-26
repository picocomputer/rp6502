/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_MSC_H_
#define _FPGA_SW_MSC_H_

#include "core/api/api.h"
#include "core/api/dir.h"
#include "core/api/std.h"

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

/* The drive's open flags. Here rather than in msc.c because the firmware
 * opens a file of its own when RP6502_LOG_FILE is on, and there must be
 * one set of these. */
#define MSC_O_READ 0x01
#define MSC_O_WRITE 0x02
#define MSC_O_CREAT 0x10
#define MSC_O_TRUNC 0x20
#define MSC_O_APPEND 0x40
#define MSC_O_EXCL 0x80

/* Lands whatever a worker left at the bridge, so the next command does
 * not stack on top of it. */
void msc_stop(void);

/* Every open file marked for rebinding, and the read cache dropped. The
 * rebinding itself is deferred to whatever asks first: eight round trips
 * inside a restore, for files a session may never touch again, is a
 * price paid whether or not it is owed. Whether the host keeps a
 * runtime binding across a restore is not documented; msc_rebind asks
 * rather than assuming. */
void msc_restore(void);
/* The drive's own count of what went wrong since it was last asked,
 * said once rather than at every failure: a stream that fails, fails
 * every frame. Silent when nothing failed. */
void msc_log(void);

/* By word index, not by slot: the table is id/size pairs and the host
 * decides where each pair lands. */

/* Blocking. */
bool msc_slot_len(uint32_t slot, uint32_t *len);

/* Converted to code page bytes, refused rather than truncated if it will
 * not fit. Blocking, and now on gameplay paths as well as staging: a
 * lazy rebind asks from inside a program's own syscall. */
bool msc_getfile(uint32_t slot, char *out, size_t cap);

/* Pull a whole .rp6502 into the staging store where the host puts one,
 * so the loader parses it the same way either arrived. Blocking, and
 * only ever called with the 6502 stopped. */
bool msc_stage_rom(const char *path, uint32_t *len);

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

/* This drive, for core/api/dir.c's handlers. */
extern const dir_backend_t msc_dir_backend;

#endif /* _FPGA_SW_MSC_H_ */
