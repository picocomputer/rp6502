/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Portable compiler idioms. Include explicitly (unlike msvc_compat.h, which
 * is force-included only under MSVC) so both the GCC/Clang and MSVC spellings are
 * visible wherever they're used.
 *
 * Packed struct (no padding), for host-side wire formats:
 *     EMU_PACK_BEGIN
 *     struct EMU_PACKED foo { ... };
 *     EMU_PACK_END
 */

#ifndef _EMU_COMPILER_H_
#define _EMU_COMPILER_H_

#if defined(_MSC_VER)
#define EMU_PACK_BEGIN __pragma(pack(push, 1))
#define EMU_PACK_END __pragma(pack(pop))
#define EMU_PACKED
#else
#define EMU_PACK_BEGIN
#define EMU_PACK_END
#define EMU_PACKED __attribute__((packed))
#endif

#endif /* _EMU_COMPILER_H_ */
