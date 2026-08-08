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
 * so it must be last in main_std_drivers: it claims every path no
 * earlier driver took, which is what the shared std.c expects of the
 * mass-storage drive on the real machine. */

/* There is no working directory on this platform, so each side of the
 * API pins its own folder. A relative path through std open resolves
 * under MSC_SAVES_PATH, where the host keeps a core's writable files; a
 * relative path naming a program resolves under MSC_ASSETS_PATH, which
 * is where the Pocket's menu browses and where slot 0 was bound from.
 * An absolute path travels untouched either way, which is why argv[0]
 * keeps the prefix the host gave it. */
#define MSC_SAVES_PATH "/Saves/rp6502/common/"
#define MSC_ASSETS_PATH "/Assets/rp6502/common/"

/* The one slot the machine reads without opening: the host stages the
 * whole image at ROM_BRIDGE. The fonts and code pages above the windows
 * are staged too, but only the fabric asks for those. */
#define MSC_SLOT_ROM 10

/* The 6502 going into reset. Lands whatever a worker left at the bridge,
 * so the next command does not stack on top of it. */
void msc_stop(void);

/* One data table word, by word index and not by slot. The table is 256
 * words of id/size pairs and the host decides where each pair lands. */
uint32_t msc_dt(uint32_t word);

/* A slot's size, by id. The host places the table's pairs where it likes,
 * so this looks the id up rather than indexing. Blocking. */
bool msc_slot_len(uint32_t slot, uint32_t *len);

/* The host's name for whatever file is bound to a slot, converted to
 * code page bytes, refused rather than truncated if it will not fit.
 * Blocking, so staging time only — this is how the core learns the name
 * of the ROM the user picked, which arrives staged and otherwise
 * anonymous. The answer lands in GETFILE_WIN's scratch. */
bool msc_getfile(uint32_t slot, char *out, size_t cap);

/* Pull a whole .rp6502 into the staging store where the host puts one,
 * so the loader parses it the same way either arrived. This is what exec
 * is: the machine staging its own next program. Blocking, and only ever
 * called with the 6502 stopped. */
bool msc_stage_rom(const char *path, uint32_t *len);

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
