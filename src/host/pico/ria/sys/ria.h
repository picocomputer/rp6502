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

/* Two of the 6502's control pins, here because this is what drives them: the
 * write state machine side-sets PHI2, and the RIA's own interrupt sources are
 * the only thing this firmware puts on IRQB (the 6522 drives its own, wired-
 * OR on the board). */
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
void ria_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac);

// Trigger IRQ when enabled
void ria_trigger_vsync(void);

// 6502 memory-mapped UART (0xFFE0-0xFFE2) <-> console bridge. The cross-core TX
// ring and RX handoff slot live in ria.c (act_loop's core); com.c (core 0)
// drains TX and feeds RX through these accessors.
bool ria_uart_tx_dequeue(uint8_t *ch); // pop one 6502-TX byte (false if empty)
bool ria_uart_tx_empty(void);          // 6502-TX ring drained?
bool ria_uart_rx_offer_ready(void);    // RX handoff slot free?
void ria_uart_rx_offer(uint8_t ch);    // hand a byte to the 6502
int ria_uart_rx_peek(void);            // peek the offered byte (-1 if none)
bool ria_uart_rx_reclaim(uint8_t *ch); // take back an unconsumed offered byte
void ria_uart_rx_clear(void);          // drop the handoff (break/stop)

// Move data from the 6502 to mbuf.
void ria_read_buf(uint16_t addr);

// Move data from mbuf to the 6502.
void ria_write_buf(uint16_t addr);

// Verify the mbuf matches 6502 memory.
void ria_verify_buf(uint16_t addr);

// Prints a "?" error and returns true if last mbuf action failed.
bool ria_handle_error(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. Its position is init
 * order and nothing more: the transfer that ria_active() reports is closed by
 * ria_task, not by ria_stop, so no other driver's stop depends on where this
 * one sits. */
#define RIA_DRIVER DRIVER(ria_init, ria_task, nul_task, ria_run, ria_stop, nul_break, nul_config, nul_config)

#endif /* _RIA_SYS_RIA_H_ */
