/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_RIA_H_
#define _VGA_SYS_RIA_H_

/* Sends real-time and status info the the RIA
 * over the rx line reversed into the backchannel.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void ria_init(void);
void ria_task(void);

void ria_flush(void);
void ria_reclock(void);
void ria_backchan(uint16_t word);
void ria_vsync(void);
void ria_ack(void);
void ria_nak(void);

#endif /* _VGA_SYS_RIA_H_ */
