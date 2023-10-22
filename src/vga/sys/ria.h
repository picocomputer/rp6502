/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_H_
#define _RIA_H_

#include <stdint.h>

void ria_init(void);
void ria_task(void);
void ria_flush(void);
void ria_reclock(void);
void ria_backchan(uint16_t word);
void ria_vsync(void);
void ria_ack(void);
void ria_nak(void);

#endif /* _RIA_H_ */
