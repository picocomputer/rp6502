/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_STD_H_
#define _CORE_API_STD_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/api/api.h"
#include "core/str/rln.h"
#include "core/sys/sst.h"

void std_init(void);
void std_task(void);
void std_stop(void);

/* Whether a cooked read is outstanding on the console, and the notice that
 * input has ended. On end of input whatever partial line was typed is still
 * delivered, so reads answer from what is left of it before every read answers
 * end of file until the next program starts.
 */

bool std_stdin_waiting(void);
void std_stdin_eof(void);

/* True once the running program has read the console. It stays true until the
 * program stops, because a raw TTY: read completes at once and leaves nothing
 * for a caller to observe. */
bool std_console_asked(void);

/* The reader rln calls back with a line. rln writes down whether a read was
 * outstanding as a token rather than as a pointer, so this is the reader its
 * load installs when the token says one was. */
rln_read_callback_t std_rln_reader(void);

bool std_api_open(void);
bool std_api_close(void);
bool std_api_read_xstack(void);
bool std_api_read_xram(void);
bool std_api_write_xstack(void);
bool std_api_write_xram(void);
bool std_api_syncfs(void);
bool std_api_lseek_cc65(void);
bool std_api_lseek_llvm(void);

typedef enum
{
    STD_OK,      /* completed, success */
    STD_ERROR,   /* failed, check errno */
    STD_PENDING, /* incomplete, would block */
} std_rw_result;

// One stdio file driver. Each machine lists its own table of these, and open()
// tries the rows in order until one claims the path with handles().
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
    // The savestate's two, optional like the rest. ident writes down what an
    // open descriptor is and reopen makes that descriptor again.
    //
    // A driver that leaves them NULL cannot carry its descriptors, so a
    // machine with one of those open refuses to be saved.
    bool (*ident)(int desc, sst_cursor_t *);
    int (*reopen)(sst_cursor_t *, api_errno *);
} std_driver_t;

const std_driver_t *std_drivers(size_t *count);

/* The descriptors above the console rows, the transfer one of them may be in
 * the middle of, and how far the stdin reader has got through a line. A
 * descriptor is written down by the driver that opened it, and its function
 * pointers are rebuilt from that driver's row number rather than carried.
 *
 * 4240 is a fixed slot with room to spare. The most that can be written is
 * sixteen carry flags, eleven carriable descriptors (fd 5 through 15) of a
 * driver byte and at most 261 bytes of ident, and a twenty byte tail, which
 * comes to 2918. */
#define STD_SST_SIZE 4240
void std_sst_save(sst_cursor_t *c, unsigned flags);
bool std_sst_load(sst_cursor_t *c, unsigned flags);

#define STD_DRIVER DRIVER(std_init, std_task, nul_task, nul_run, std_stop, nul_break, \
    nul_config, nul_config, SST(STD_, 1, STD_SST_SIZE, std_sst_save, std_sst_load))

#endif /* _CORE_API_STD_H_ */
