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

#define FS_DRIVE "FS:"

/* A relative name resolves under FS_SAVES_PATH, the working directory, as
 * a name on SAVE: does. The ROMs are in FS_ASSETS_PATH, and the relative
 * path of an exec resolves there instead. */
#define FS_SAVES_PATH "/Saves/rp6502/common"
#define FS_ASSETS_PATH "/Assets/rp6502/common"

/* The ROM is the first slot in data.json because a hot reload writes the
 * new image through the first slot record. */
#define FS_SLOT_ROM 0

/* FS_DESC_ROM is one past the eight descriptors that fs_std_open
 * allocates, so it never collides with one of them. */
#define FS_DESC_ROM 8

int fs_rom_adopt(api_errno *err);

void fs_stop(void);

/* No pocket_file command is in flight. A command spans passes of the main
 * loop, and a savestate is made only between them. */
bool fs_idle(void);

/* File commands that ended in an error or a timeout since boot. */
uint32_t fs_failed_count(void);

bool fs_slot_len(uint32_t slot, uint32_t *len);

bool fs_getfile(uint32_t slot, char *out, size_t cap);

const char *fs_strip_drive(const char *path);

/* fs_card_path writes the absolute card path for path, which has FS:
 * already stripped. A relative path is taken under root, and . and .. are
 * resolved. It returns false when the result does not fit in cap. */
bool fs_card_path(const char *path, const char *root, char *out, size_t cap);

#define FS_POCKET_DRIVER DRIVER(STOP(fs_stop))

#endif /* _HOST_POCKET_SW_FS_H_ */
