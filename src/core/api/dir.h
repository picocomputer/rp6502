/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The eighteen directory syscalls, as the 6502 makes them.
 *
 * What reaches it is the same on every machine -- the xstack a path arrives
 * on, the f_stat_t an entry leaves in, which errno a bad descriptor is -- so
 * it is written here once, over whatever drive a machine has. That drive is
 * osal/dir.h.
 *
 * The entry counter telldir and seekdir are about is this layer's. Both drives
 * kept one, both counted the same things, and a drive that skips "." and ".."
 * on its own gets the counting right by saying nothing about it.
 */

#ifndef _CORE_API_DIR_H_
#define _CORE_API_DIR_H_

#include "osal/dir.h"
#include "core/sys/sst.h"

/* Machine events: a run starts with no directory open. */
void dir_run(void);
void dir_stop(void);

bool dir_api_stat(void);
bool dir_api_opendir(void);
bool dir_api_readdir(void);
bool dir_api_closedir(void);
bool dir_api_telldir(void);
bool dir_api_seekdir(void);
bool dir_api_rewinddir(void);
bool dir_api_unlink(void);
bool dir_api_rename(void);
bool dir_api_chmod(void);
bool dir_api_utime(void);
bool dir_api_mkdir(void);
bool dir_api_chdir(void);
bool dir_api_chdrive(void);
bool dir_api_getcwd(void);
bool dir_api_setlabel(void);
bool dir_api_getlabel(void);
bool dir_api_getfree(void);

/* Every open directory, by the path it is reading and the entry it has read
 * up to, and the working directory all of them and every file syscall resolve
 * against. A directory is reopened and wound rather than restored, because
 * what a drive holds open is the host's own object and no blob can carry it.
 *
 * 8 tells of 4, 8 slots of a flag and a path, then the cwd. */
#define DIR_SST_SIZE (8 * 4 + 8 * (1 + API_PATH_MAX + 1) + API_PATH_MAX + 1)
void dir_sst_save(sst_cursor_t *c, unsigned flags);
bool dir_sst_load(sst_cursor_t *c, unsigned flags);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define DIR_DRIVER DRIVER(nul_init, nul_task, nul_task, dir_run, dir_stop, nul_break, \
    nul_config, nul_config, SST(DIR_, 1, DIR_SST_SIZE, dir_sst_save, dir_sst_load))

#endif /* _CORE_API_DIR_H_ */
