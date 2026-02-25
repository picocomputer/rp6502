
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
#include "api/api.h"
#include "api/std.h"

/* Main events
 */

void rom_init(void);
void rom_task(void);
void rom_break(void);
void rom_stop(void);

// True when more work is pending.
bool rom_active(void);

/* Monitor commands
 */

void rom_mon_load(const char *args, size_t len);
void rom_mon_info(const char *args, size_t len);
void rom_mon_install(const char *args, size_t len);
void rom_mon_remove(const char *args, size_t len);
void rom_mon_help(const char *args, size_t len);

// Begin loading an installed rom, if exists.
bool rom_load_installed(const char *args, size_t len);

// Responder prints all installed ROMs.
int rom_installed_response(char *buf, size_t buf_size, int state);

// Configuration setting BOOT
// No loader because this isn't stored in RAM
bool rom_set_boot(char *str);
const char *rom_get_boot(void); // uses mbuf

/* STDIO 
 */

bool rom_std_handles(const char *path);
int rom_std_open(const char *path, uint8_t flags, api_errno *err);
int rom_std_close(int desc, api_errno *err);
std_rw_result rom_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result rom_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);
int rom_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err);

#endif /* _RIA_MON_ROM_H_ */
