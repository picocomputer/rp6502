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
#include "core/sys/sst.h"

void std_init(void);
void std_task(void);

/* A host driver calls this as data of a read lands in memory. The bytes that
 * fall in the range of a running read_xram are forwarded from then on, not
 * only after the driver's read returns. */
void std_xram_landed(const void *dst, uint32_t n);
void std_stop(void);

/* Every open file closed and the console on 0, 1 and 2 again, CON:0, CON:1
 * and CON:2. The END walk runs it, at the end of an exec chain and before a
 * launcher starts again, not at each stop, so a program that an exec starts
 * has what the one before it left open. */
void std_reset(void);

bool std_api_open(void);
bool std_api_close(void);
bool std_api_dup(void);
bool std_api_dup2(void);
bool std_api_isatty(void);

// Whether a descriptor open now refuses an exec (std_driver_t).
bool std_exec_refused(void);
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
    // opening returns STD_PENDING while the descriptor from open is not ready
    // (re-dispatched on schedule), STD_OK once it is, STD_ERROR when the open
    // failed (check errno), and the core then closes the descriptor
    std_rw_result (*opening)(int desc, api_errno *);
    std_rw_result (*read)(int desc, char *, uint32_t, uint32_t *, api_errno *);
    std_rw_result (*write)(int desc, const char *, uint32_t, uint32_t *, api_errno *);
    std_rw_result (*sync)(int desc, api_errno *);
    int (*lseek)(int desc, int8_t, int32_t, int32_t *, api_errno *);
    // A file of the filesystem, SAVE: included: a machine has STD_FILES_MAX
    // of them open at once.
    bool file;
    // An open one refuses an exec. ROM: reads the running program's own
    // image, and SAVE: is in the running program's save folder, neither of
    // which is the next program's.
    bool refuses_exec;
    // The console, which isatty answers 1 for.
    bool terminal;
    // The savestate's part, optional like the rest.
    struct
    {
        // ident writes down what an open descriptor is and reopen makes that
        // descriptor again, with the open mode that the core carries. A
        // driver that leaves them NULL cannot carry its descriptors, so a
        // machine with one of those open refuses to be saved.
        bool (*ident)(int desc, sst_cursor_t *);
        int (*reopen)(sst_cursor_t *, uint8_t mode, api_errno *);
        // A descriptor open for writing refuses a savestate unless its
        // driver sets this. A write in progress, such as a game saving its
        // progress, cannot be caught whole, but sound can always resume.
        bool resumes_writes;
    } sst;
} std_driver_t;

const std_driver_t *std_drivers(size_t *count);

/* Whether an open file refuses a savestate: one open for writing, on a
 * driver that does not resume writes. Every savestate asks here, core's own
 * and one a machine makes without core/sys/sst.c. */
bool std_sst_refused(void);

/* The open files, the descriptors that name them, the transfer, open or dup2
 * one of them may be in the middle of. An open file is written down by the
 * driver that opened it, and its handlers are found again from that driver's
 * row number and the open mode rather than carried.
 *
 * The most that can be written is sixteen carry flags, sixteen open files of
 * a driver byte, a mode byte and at most 260 bytes of ident, sixteen
 * descriptors and a fourteen byte tail, which comes to 4238. */
#define STD_SST_SIZE 4400
void std_sst_save(sst_cursor_t *c, unsigned flags);
bool std_sst_load(sst_cursor_t *c, unsigned flags);

#define STD_DRIVER DRIVER(                                  \
    INIT(std_init)                                          \
    TASK_PUMPED(std_task)                                   \
    STOP(std_stop)                                          \
    END(std_reset)                                          \
    SST(.id = "STD_",                                       \
        .version = 4,                                       \
        .size = STD_SST_SIZE,                               \
        .save = std_sst_save,                               \
        .load = std_sst_load))

#endif /* _CORE_API_STD_H_ */
