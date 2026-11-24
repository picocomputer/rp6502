/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_MODE6_H_
#define _CORE_VGA_MODE6_H_

#include <stdbool.h>
#include <stdint.h>

/* Mode 6, the program's pointers: CONFIG, the XRAM address of an array of
 * mode 5 custom sprites, the pointer's shape n drawing sprite n - 1 with
 * its x and y the hot spot, or $FFFF for the system's pointers. It is not
 * a plane, so it outlives a change between the program's canvases; going
 * back to the console puts the system's pointers back. */
bool mode6_prog(uint16_t *xregs);

#endif /* _CORE_VGA_MODE6_H_ */
