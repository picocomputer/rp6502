/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Forced into every translation unit under MSVC with /FIcompat.h, so it has no
 * include guard of its own.
 */

#ifdef _MSC_VER
#ifndef __attribute__
#define __attribute__(x)
#endif
#endif
