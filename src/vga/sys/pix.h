/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PIX_H_
#define _PIX_H_

#include <stdint.h>

#define PIX_XREGS_MAX 8
extern volatile uint8_t pix_xregs[PIX_XREGS_MAX];

void pix_init(void);
void pix_task(void);

#endif /* _PIX_H_ */
