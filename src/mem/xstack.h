
/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _XSTACK_H_
#define _XSTACK_H_

#include <stddef.h>
#include <stdint.h>

#define XSTACK_SIZE 0x100
extern uint8_t xstack[];
extern volatile size_t xstack_ptr;

#endif /* _XSTACK_H_ */
