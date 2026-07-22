/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for <pico.h>: the flash-placement attributes (no-ops on the host),
 * <assert.h> (the real pico.h pulls C11 static_assert), and the pico compiler
 * macros the shared firmware sources use (the real pico.h pulls
 * pico/platform/compiler.h). Included by the vendored FatFs ffunicode.c and the
 * reused firmware ria/ sources (dir.c, str.c).
 */

#ifndef _EMU_SHIM_PICO_H_
#define _EMU_SHIM_PICO_H_

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define __in_flash(...)
#define __not_in_flash(...)
#define __not_in_flash_func(func) func
#define __in_flash_func(func) func

/* pico/platform/compiler.h supplies these; mirror them for the host. glibc's
 * __CONCAT expands its args only once, so override it with the two-level form
 * str.c's string tables rely on. */
#ifndef __STRING
#define __STRING(x) #x
#endif
#ifndef __XSTRING
#define __XSTRING(x) __STRING(x)
#endif
#ifndef __CONCAT1
#define __CONCAT1(a, b) a##b
#endif
#undef __CONCAT
#define __CONCAT(a, b) __CONCAT1(a, b)
#ifndef __printflike
#ifdef __GNUC__
#define __printflike(a, b) __attribute__((__format__(__printf__, a, b)))
#else
#define __printflike(a, b)
#endif
#endif

#endif /* _EMU_SHIM_PICO_H_ */
