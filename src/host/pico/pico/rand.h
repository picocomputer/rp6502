/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for the pico-sdk pico/rand.h. The vendored firmware atr.c pulls
 * entropy from get_rand_64; the emulator backs it with host entropy (rand.c),
 * or a fixed seed via rand_set_seed. Only the one entry the firmware
 * uses is declared.
 */

#ifndef _HOST_PICO_PICO_RAND_H_
#define _HOST_PICO_PICO_RAND_H_

#include <stdint.h>

uint64_t get_rand_64(void);

#endif /* _HOST_PICO_PICO_RAND_H_ */
