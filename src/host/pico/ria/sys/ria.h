/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_RIA_H_
#define _RIA_SYS_RIA_H_

/* RP6502 Interface Adapter for WDC W65C02S.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define RIA_PIN_BASE 6
#define RIA_CS_PIN (RIA_PIN_BASE + 0)
#define RIA_RWB_PIN (RIA_PIN_BASE + 1)
#define RIA_DATA_PIN_BASE (RIA_PIN_BASE + 2)  /* D0-D7 */
#define RIA_ADDR_PIN_BASE (RIA_PIN_BASE + 10) /* A0-A4 */

#define CPU_PHI2_PIN 21
#define CPU_IRQB_PIN 22

#define RIA_CS_RWB_PIO pio0
#define RIA_CS_RWB_SM 0
#define RIA_WRITE_PIO pio0
#define RIA_WRITE_SM 1
#define RIA_READ_PIO pio0
#define RIA_READ_SM 2
#define RIA_ACT_PIO pio1
#define RIA_ACT_SM 0

#include "core/sys/driver.h"

/* Main events
 */

void ria_init(void);
void ria_task(void);
void ria_run(void);
void ria_stop(void);
void ria_break(void);
void ria_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac);

// Trigger IRQ when enabled
void ria_trigger_vsync(void);

bool ria_uart_tx_dequeue(uint8_t *ch);
bool ria_uart_tx_empty(void);
bool ria_uart_rx_offer_ready(void);
void ria_uart_rx_offer(uint8_t ch);
int ria_uart_rx_peek(void);
bool ria_uart_rx_reclaim(uint8_t *ch);

/* A transfer copies between mbuf and 6502 memory by resetting the 6502 into a
 * loop at $FFF0, so it must be started while the machine is stopped. The
 * callback is called from ria_task after any timeout or verify error has been
 * queued, and a write that finishes is verified before its callback is
 * called. A break closes the transfer without calling the callback. */
typedef void (*ria_callback_t)(bool ok);
void ria_read_buf(uint16_t addr, ria_callback_t callback);
void ria_write_buf(uint16_t addr, ria_callback_t callback);

#define RIA_DRIVER DRIVER(ria_init, ria_task, nul_task, ria_run, ria_stop, ria_break, nul_config, nul_config, nul_sst)

#endif /* _RIA_SYS_RIA_H_ */
