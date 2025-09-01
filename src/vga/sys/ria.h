/*
 * Copyright (c) 2025 Rumbledethumps
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

// PIX is unidirectional and we're out of pins.
// The RIA also sends UART data over PIX so we can
// reconfigure that pin for a return channel.
#define RIA_BACKCHAN_PIN COM_UART_RX_PIN
#define RIA_BACKCHAN_BAUDRATE 115200
#define RIA_BACKCHAN_PIO pio1
#define RIA_BACKCHAN_SM 3

/* Main events
 */

void ria_init(void);
void ria_task(void);
void ria_pre_reclock(void);
void ria_post_reclock(void);

/* Utility
 */

void ria_backchan(uint16_t word);
void ria_vsync(void);
void ria_ack(void);
void ria_nak(void);

#endif /* _VGA_SYS_RIA_H_ */
