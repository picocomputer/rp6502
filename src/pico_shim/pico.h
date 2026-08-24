/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for <pico.h>, for vendor/fatfs/ffunicode.c and nothing else.
 *
 * That file is our patched copy: it places its code page tables with
 * __in_flash, which is a Pico's word. Our own sources ask their host where
 * to put things -- see HOST_IN_FLASH in core/host.h -- so this exists only
 * to keep a vendored file compiling off-target, and shrinks whenever that
 * file needs less.
 */

#ifndef _PICO_SHIM_PICO_H_
#define _PICO_SHIM_PICO_H_

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

#endif /* _PICO_SHIM_PICO_H_ */
