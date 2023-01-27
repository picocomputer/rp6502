
/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MBUF_H_
#define _MBUF_H_

#include <stddef.h>
#include <stdint.h>

// Buffer for moving things around.
// 6502 <-> RAM, USB <-> RAM, UART <-> RAM, etc.

#define MBUF_SIZE 1024
extern uint8_t mbuf[MBUF_SIZE];
extern size_t mbuf_len;
uint32_t mbuf_crc32();

#endif /* _MBUF_H_ */
