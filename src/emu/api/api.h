/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * RIA fastcall register aliases (the $FFEx API window) and the I/O poll result.
 * The api_* return/marshal helpers themselves are declared by the firmware's
 * api/api.h, which api.c implements; this header carries only the emulator-side
 * register aliases and the shared io_result type the std/file drivers return.
 */

#ifndef _EMU_API_H_
#define _EMU_API_H_

#include "emu/sys/mem.h" /* REGS / REGSW */

#ifdef __cplusplus
extern "C"
{
#endif

/* RIA fastcall register aliases (match ria/api/api.h). */
#define API_OP REGS(0xFFEF)
#define API_ERRNO REGSW(0xFFED)
#define API_STACK REGS(0xFFEC)
#define API_A REGS(0xFFF4)
#define API_X REGS(0xFFF6)
#define API_SREG REGSW(0xFFF8)

#define FS_HOST_MAX_PATH 4096 /* host path buffer size for fs_to_host callers */

/* Poll status for the I/O API: a source returns IO_PENDING to be polled again
 * (stdin until a line; an AIO transfer until it completes), IO_OK once this
 * poll's bytes are in (got may be 0 at EOF), IO_ERROR on failure (errno set).
 * Under the real-time window both MSC0: and ROM: reads run as POSIX AIO and
 * may return IO_PENDING; headless they complete synchronously. */
typedef enum { IO_OK, IO_PENDING, IO_ERROR } io_result;

/* Reset per-run API state (the errno-option mapping). */
void api_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_API_H_ */
