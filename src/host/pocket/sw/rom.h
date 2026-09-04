/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_ROM_H_
#define _HOST_POCKET_SW_ROM_H_

#include <stdbool.h>

/* Load a .rp6502 through the ROM descriptor into the fabric. The path is
 * the host's spelling of a card file; the staging pull, the parse and the
 * ROM: assets all ride the one descriptor underneath. True when the
 * program and its reset vector are in place. */
bool rom_load(const char *path);

/* Over a descriptor already held -- the boot's, adopted from the host. */
bool rom_load_fd(int fd);

#endif /* _HOST_POCKET_SW_ROM_H_ */
