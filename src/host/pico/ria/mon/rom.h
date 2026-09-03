
/*
 * Copyright (c) 2026 Rumbledethumps
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
#include "core/api/api.h"
#include "core/api/std.h"

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

void rom_mon_load(const char *args);
/* The load below LOAD's argument gate: argv0 verbatim plus parsed args.
 * NFC feeds installed names through here; the open answers for them. */
void rom_load_argv(const char *argv0, const char *args);
void rom_mon_info(const char *args);
void rom_mon_install(const char *args);
void rom_mon_remove(const char *args);
void rom_mon_help(const char *args);

// Begin loading an installed rom, if exists.
bool rom_load_installed(const char *args);

// Begin loading the ROM from argv[0].
void rom_exec(void);

// Responder prints all installed ROMs.
int rom_installed_response(char *buf, size_t buf_size, int state, unsigned width);

// Configuration setting BOOT
// No loader because this isn't stored in RAM
// Accepts the full argument string (may include args after the ROM name).
bool rom_set_boot(const char *args);
const char *rom_get_boot(void); // uses mbuf

/* STDIO 
 */

int rom_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result rom_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define ROM_DRIVER DRIVER(rom_init, nul_task, rom_task, nul_run, rom_stop, rom_break, nul_config, nul_config)


#endif /* _RIA_MON_ROM_H_ */
