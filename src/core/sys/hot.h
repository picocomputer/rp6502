/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The code the Pico runs every pixel or every sample, built for speed
 * whatever the build's own level.
 *
 *   HOT_BEGIN ... HOT_END   what lies between is optimized at -O3
 *   HOT_UNROLL(n)           the loop that follows is unrolled n times
 *
 * They are GCC's pragmas. Clang has the unroll alone, and MSVC none of
 * them; their builds keep their own level. */

#ifndef _CORE_SYS_HOT_H_
#define _CORE_SYS_HOT_H_

#define HOT_PRAGMA(x) _Pragma(#x)
#if defined(__GNUC__) && !defined(__clang__)
#define HOT_BEGIN HOT_PRAGMA(GCC push_options) HOT_PRAGMA(GCC optimize("O3"))
#define HOT_END HOT_PRAGMA(GCC pop_options)
#else
#define HOT_BEGIN
#define HOT_END
#endif
#if defined(__GNUC__) || defined(__clang__)
#define HOT_UNROLL(n) HOT_PRAGMA(GCC unroll n)
#else
#define HOT_UNROLL(n)
#endif

#endif /* _CORE_SYS_HOT_H_ */
