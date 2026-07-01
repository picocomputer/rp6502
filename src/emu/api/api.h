/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * RIA fastcall register aliases (the $FFEx API window). The api_* return/marshal
 * helpers themselves are declared by the firmware's api/api.h and implemented by
 * the shared ria/api/api.c; this header carries only the emulator-side register
 * aliases. The std/file drivers return the firmware's std_rw_result (see api/std.h).
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

#ifdef __cplusplus
}
#endif

#endif /* _EMU_API_H_ */
