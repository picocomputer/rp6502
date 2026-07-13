/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Forced include under MSVC so shared RIA/VGA sources that use GCC attributes
 * still compile. Keep empty — do not invent new APIs here.
 */

#ifdef _MSC_VER
#ifndef __attribute__
#define __attribute__(x)
#endif
#endif
