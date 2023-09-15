/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MODES_H_
#define _MODES_H_

#include <stdbool.h>
#include <stdint.h>

#define SCANPROG_MAX 512
typedef struct
{
    void (*fn320[3])(void *ctx, int16_t scanline, uint16_t *rgb);
    void (*fn640[3])(void *ctx, int16_t scanline, uint16_t *rgb);
    void *ctx[3];
} scanprog_t;

extern scanprog_t scanprog[SCANPROG_MAX];

bool mode_mode(uint16_t *xregs);
void mode_init(void);

#endif /* _MODES_H_ */
