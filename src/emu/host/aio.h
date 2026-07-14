/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A single in-flight async transfer (POSIX AIO where the platform provides it,
 * EMU_HAVE_AIO from CMake's aio_read probe). The host drives keep the 6502
 * clocking while a read/write completes by submitting into an aio_slot and
 * polling it from the per-scanline pump. Where AIO is absent (Windows, web) the
 * slot is inert and callers fall back to synchronous fs_* I/O.
 *
 * This is the one place the aiocb machinery lives; msc.c/rom.c embed a slot and
 * never touch <aio.h> directly.
 */

#ifndef _EMU_HOST_AIO_H_
#define _EMU_HOST_AIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef EMU_HAVE_AIO
#include <aio.h>
#include <unistd.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/* EMU_HAVE_AIO is PRIVATE to emu_core, so this struct's size differs across the
 * macro boundary. It is only ever embedded inside msc.c/rom.c (emu_core TUs);
 * app/test TUs must call aio_set_enabled() and nothing else here. */
struct aio_slot
{
#ifdef EMU_HAVE_AIO
    bool active;
    struct aiocb cb; /* the single in-flight transfer */
#else
    char unused; /* an empty struct isn't standard C */
#endif
};

/* Async transfers on/off globally. Off by default: headless/tests and the web
 * build do synchronous I/O. Always false where EMU_HAVE_AIO is unset. */
void aio_set_enabled(bool on);
bool aio_enabled(void);

bool aio_active(const struct aio_slot *s);

/* Begin a positioned transfer; only when aio_enabled() && !aio_active(). Returns
 * false with errno set if the submit failed. */
bool aio_submit_read(struct aio_slot *s, int fd, int64_t off, void *buf, size_t n);
bool aio_submit_write(struct aio_slot *s, int fd, int64_t off, const void *buf, size_t n);

/* Poll the in-flight transfer: 1 = done (*got set), 0 = still running,
 * -1 = error (errno set to the async failure). */
int aio_poll(struct aio_slot *s, size_t *got);

/* Cancel and reap any in-flight transfer (a reset can close mid-op). Uses the
 * still-open fd, so call it before closing. Idempotent; no-op if idle. */
void aio_slot_cancel(struct aio_slot *s, int fd);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_AIO_H_ */
