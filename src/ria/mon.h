/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MON_H_
#define _MON_H_

#include <stdint.h>

void mon_task();
void mon_read(uint16_t addr, uint8_t *buf, uint16_t len);
void mon_write(uint16_t addr, uint8_t *buf, uint16_t len);
void mon_reset();

#endif /* _MON_H_ */
