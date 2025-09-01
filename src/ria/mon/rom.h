
/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MON_ROM_H_
#define _RIA_MON_ROM_H_

/* Monitor commands for working with ROM (*.rp6502) files.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void rom_init(void);
void rom_task(void);
void rom_break(void);

// True when more work is pending.
bool rom_active(void);

/* Monitor commands
 */

void rom_mon_load(const char *args, size_t len);
void rom_mon_info(const char *args, size_t len);
void rom_mon_install(const char *args, size_t len);
void rom_mon_remove(const char *args, size_t len);

// Begin loading an installed rom, if exists.
bool rom_load_installed(const char *args, size_t len);

// Display help from an installed ROM.
bool rom_help(const char *args, size_t len);

#endif /* _RIA_MON_ROM_H_ */
