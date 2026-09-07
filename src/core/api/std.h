/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_STD_H_
#define _CORE_API_STD_H_

/* Provides STDIO to the 6502.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/api/api.h"
#include "core/str/rln.h"
#include "core/sys/sst.h"

/* Main events
 */

void std_init(void);
void std_task(void);
void std_stop(void);

/* A host's stdin, where there is one: whether a cooked read is waiting on
 * it, and that it has run out -- the read in progress answers nothing and
 * every read after answers end of file, until the next program.
 */

bool std_stdin_waiting(void);
void std_stdin_eof(void);

/* Whether this program has read the console at all -- a latch, because a
 * raw TTY: read is not outstanding between calls. */
bool std_console_asked(void);

/* The one line reader a software machine has. rln carries whether a read was
 * outstanding as a token rather than a pointer, and this is what it installs
 * the token back as. */
rln_read_callback_t std_rln_reader(void);

/* The API implementation for stdio support.
 */

bool std_api_open(void);
bool std_api_close(void);
bool std_api_read_xstack(void);
bool std_api_read_xram(void);
bool std_api_write_xstack(void);
bool std_api_write_xram(void);
bool std_api_syncfs(void);
bool std_api_lseek_cc65(void);
bool std_api_lseek_llvm(void);

/* Driver I/O result codes for read/write operations
 */

typedef enum
{
    STD_OK,      /* completed, success */
    STD_ERROR,   /* failed, check errno */
    STD_PENDING, /* incomplete, would block */
} std_rw_result;

// One stdio file driver. The open dispatcher claims a path with the first
// driver whose handles() returns true, so an inactive driver simply returns
// false. Each machine builds its own table from this struct.
typedef struct
{
    // handles, open, and close are required
    bool (*handles)(const char *);
    int (*open)(const char *, uint8_t, api_errno *);
    // close and sync return STD_PENDING while draining (re-dispatched on
    // schedule), STD_OK when done, STD_ERROR on failure (check errno)
    std_rw_result (*close)(int desc, api_errno *);
    // everything else is optional
    std_rw_result (*read)(int desc, char *, uint32_t, uint32_t *, api_errno *);
    std_rw_result (*write)(int desc, const char *, uint32_t, uint32_t *, api_errno *);
    std_rw_result (*sync)(int desc, api_errno *);
    int (*lseek)(int desc, int8_t, int32_t, int32_t *, api_errno *);
    // A savestate's two, and optional like the rest. ident writes down what an
    // open descriptor is; reopen reads that back and makes the descriptor
    // again. What a driver writes is its own -- a filesystem needs a name, an
    // access and an offset, while a ROM: window is three numbers and no name
    // at all -- so the two speak in bytes rather than in a shape.
    //
    // A driver that leaves them NULL cannot carry its descriptors. A machine
    // with one of those open refuses to be saved, rather than come back
    // holding nothing.
    bool (*ident)(int desc, sst_cursor_t *);
    int (*reopen)(sst_cursor_t *, api_errno *);
} std_driver_t;

/* This machine's driver table, in the order open() tries them. std.c builds
 * it from the RP6502_STD_DRIVERS rows the machine's drivers.h lists; the
 * accessor is declared beside the struct it hands back. */
const std_driver_t *std_drivers(size_t *count);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
/* Every open descriptor, the transfer one of them may be in the middle of,
 * and the stdin bridge's own place in a line the reader has not finished.
 *
 * A descriptor is written down by the driver that opened it -- a name, an
 * access and an offset for a file, three numbers for a ROM: window -- so the
 * five function pointers in the pool are rebuilt from a driver index rather
 * than carried. The manifest covers the stdio roster for that reason: an
 * index means nothing to a machine that lists its drivers differently.
 *
 * The slot is the worst case, sixteen descriptors each naming a path. */
#define STD_SST_SIZE 4240
void std_sst_save(sst_cursor_t *c, unsigned flags);
bool std_sst_load(sst_cursor_t *c, unsigned flags);

#define STD_DRIVER DRIVER(std_init, std_task, nul_task, nul_run, std_stop, nul_break, \
    nul_config, nul_config, SST(STD_, 1, STD_SST_SIZE, std_sst_save, std_sst_load))

#endif /* _CORE_API_STD_H_ */
