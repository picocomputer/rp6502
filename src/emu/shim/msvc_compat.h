/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Forced include under MSVC (/FImsvc_compat.h) so shared RIA/VGA/emulator sources
 * written for GCC/Clang still compile. Only neutralize GCC-isms MSVC lacks — do
 * not add project APIs here.
 */

#ifdef _MSC_VER
#ifndef __attribute__
#define __attribute__(x)
#endif
#ifndef __builtin_expect
#define __builtin_expect(x, v) (x)
#endif
#endif
