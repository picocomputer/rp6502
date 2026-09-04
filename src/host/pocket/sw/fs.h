/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_FS_H_
#define _HOST_POCKET_SW_FS_H_

#include "core/api/api.h"
#include "osal/dir.h"
#include "osal/fs.h"
#include "core/api/std.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The host's filesystem, over APF data slots.
 *
 * fs.c implements osal/fs.h's driver -- the catch-all, so it must be last
 * in this machine's table -- over the bridge and the slot pool declared here.
 * dir.c answers what little of a drive a data slot can be.
 *
 * Slot 0 is the ROM, which data.json puts first because a hot reload writes
 * the new image through the first slot record; the eight file descriptors
 * take slots 1..8. */

/* No working directory on this platform, so each side of the API pins
 * its own folder: std open resolves relative paths under SAVES, a
 * program name under ASSETS. An absolute path travels untouched. */
#define FS_SAVES_PATH "/Saves/rp6502/common/"
#define FS_ASSETS_PATH "/Assets/rp6502/common/"

/* First in data.json, because a hot reload writes the new image through
 * the first slot record. */
#define FS_SLOT_ROM 0

/* The single ROM descriptor: fs_rom_open/fs_rom_adopt hand it out, the
 * fs_std calls serve it from the staging store, and the 6502 can never
 * be given it -- fs_std_open never counts past its pool of 8. */
#define FS_DESC_ROM 8

/* Take the descriptor over the image the host staged itself -- a boot,
 * where slot 0 was bound and written before anything ran. */
int fs_rom_adopt(api_errno *err);

/* Lands whatever a worker left at the bridge, so the next command does
 * not stack on top of it. */
void fs_stop(void);

/* Every open file marked for rebinding, and the read cache dropped. The
 * rebinding itself is deferred to whatever asks first: eight round trips
 * inside a restore, for files a session may never touch again, is a
 * price paid whether or not it is owed. Whether the host keeps a
 * runtime binding across a restore is not documented; fs_rebind asks
 * rather than assuming. */
void fs_restore(void);

/* Drop a descriptor without telling the host. */
void fs_release(int desc);
/* The drive's own count of what went wrong since it was last asked,
 * said once rather than at every failure: a stream that fails, fails
 * every frame. Silent when nothing failed. */
void fs_log(void);

/* By word index, not by slot: the table is id/size pairs and the host
 * decides where each pair lands. */

/* Blocking. */
bool fs_slot_len(uint32_t slot, uint32_t *len);

/* Converted to code page bytes, refused rather than truncated if it will
 * not fit. Blocking, and now on gameplay paths as well as staging: a
 * lazy rebind asks from inside a program's own syscall. */
bool fs_getfile(uint32_t slot, char *out, size_t cap);

/* The length of the image behind the ROM descriptor. After a restore that
 * is the blob's answer, and the store is supposed to be holding that many
 * bytes of that program. */
uint32_t fs_rom_staged_len(void);

/* Past this machine's drive prefix, or NULL for a drive that is not this one.
 * Shared with dir.c, which has to answer chdrive the same way an open does.
 * Deliberately not core/str/path.c's: that one hands back an unrecognized
 * prefix unchanged, and this one refuses it. */
const char *fs_strip_drive(const char *path);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define FS_DRIVER DRIVER(nul_init, nul_task, nul_task, nul_run, fs_stop, nul_break, nul_config, nul_config)

#endif /* _HOST_POCKET_SW_FS_H_ */
