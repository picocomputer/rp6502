/*
 * Copyright (c) 2025 Rumbledethumps
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

#define RIA_CS_RWB_PIO pio0
#define RIA_CS_RWB_SM 0
#define RIA_WRITE_PIO pio0
#define RIA_WRITE_SM 1
#define RIA_READ_PIO pio0
#define RIA_READ_SM 2
#define RIA_ACT_PIO pio1
#define RIA_ACT_SM 0

/* Main events
 */

void ria_init(void);
void ria_task(void);
void ria_run();
void ria_stop();
void ria_post_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac);

// Trigger IRQ when enabled
void ria_trigger_irq(void);

// Move data from the 6502 to mbuf.
void ria_read_buf(uint16_t addr);

// Move data from mbuf to the 6502.
void ria_write_buf(uint16_t addr);

// Verify the mbuf matches 6502 memory.
void ria_verify_buf(uint16_t addr);

// The RIA is active when it's performing an mbuf action.
bool ria_active();

// Prints a "?" error and returns true if last mbuf action failed.
bool ria_print_error_message();

// Compute CRC32 of mbuf to match zlib.
uint32_t ria_buf_crc32();

#endif /* _RIA_SYS_RIA_H_ */
