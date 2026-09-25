/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OSAL_EMSCRIPTEN_OS_H_
#define _OSAL_EMSCRIPTEN_OS_H_

/* The browser completes these storage calls in a callback on a later turn of
 * its event loop, and the emulator cannot block that loop, so each one here is
 * a start and a poll, and osal/posix returns STD_PENDING until the poll
 * returns something else. */

#include "core/api/std.h"
#include <stdint.h>

/* FS.syncfs(false), which writes every IDBFS mount out to IndexedDB. A start
 * while one is in flight queues one more to follow it, so the last to finish
 * covers every write made before the start. The poll returns STD_PENDING
 * until every sync started so far has finished, then STD_OK, or STD_ERROR
 * with API_EIO when one of them failed. A result other than STD_PENDING is
 * returned once and ends the request; with no request outstanding the poll
 * returns STD_OK. */
void os_syncfs_start(void);
std_rw_result os_syncfs_poll(api_errno *err);

/* navigator.storage.estimate(), kept between calls because an estimate is
 * slow and changes only when something is stored. os_estimate_stale marks the
 * kept estimate out of date, and a write, a close after a write and a syncfs
 * each call it. os_estimate_start starts a new estimate only when the kept one
 * is out of date or missing, none is in flight and no failure is still
 * unreported. The poll returns STD_PENDING while an estimate is in flight, then
 * STD_OK with the quota and the usage in bytes, or STD_ERROR with API_ENOSYS
 * when the browser offers no estimate and API_EIO when it refused one. A
 * failure is returned by one poll, and the start after that poll tries
 * again. */
void os_estimate_stale(void);
void os_estimate_start(void);
std_rw_result os_estimate_poll(uint64_t *quota, uint64_t *usage, api_errno *err);

#endif /* _OSAL_EMSCRIPTEN_OS_H_ */
