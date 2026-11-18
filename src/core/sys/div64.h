/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_SYS_DIV64_H_
#define _CORE_SYS_DIV64_H_

#include <stdint.h>

/* n / d, with n % d through rem where rem is not NULL, for a d that is not
 * 0. No step of it is wider than 32 bits, so it never calls the compiler's
 * 64-bit divide, which is 2.6 KB of a firmware whose processor has no such
 * instruction. Core divides 64 bits by this and not by /, so that a
 * machine's firmware need not have those routines at all.
 *
 * It is quickest for a d that is under 65536 once its factors of two are
 * out of it, as 1000000 and a sample rate are: four 32-bit divides. Any
 * other d takes a step for each of 32 bits. */
uint64_t div64(uint64_t n, uint32_t d, uint32_t *rem);

/* n / d for a d of 64 bits that is not 0, where the answer is known to be
 * small, as a place on a canvas is: an answer that does not fit in `bits`
 * bits comes back as the largest that does. A step for each of the bits. */
uint32_t div64_small(uint64_t n, uint64_t d, unsigned bits);

#endif /* _CORE_SYS_DIV64_H_ */
