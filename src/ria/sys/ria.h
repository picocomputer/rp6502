/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_H_
#define _RIA_H_

/*
 * RP6502 Interface Adapter for WDC W65C02S.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Content of these 15 pins is bound to the PIO program structure.
#define RIA_PIN_BASE 6
#define RIA_CS_PIN (RIA_PIN_BASE + 0)
#define RIA_RWB_PIN (RIA_PIN_BASE + 1)
#define RIA_DATA_PIN_BASE (RIA_PIN_BASE + 2)
#define RIA_ADDR_PIN_BASE (RIA_PIN_BASE + 10)

// Kernel events
void ria_init();
void ria_task();
void ria_run();
void ria_stop();
void ria_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac);

// Move data from the 6502 to mbuf.
void ria_read_mbuf(uint16_t addr);

// Move data from mbuf to the 6502.
void ria_write_mbuf(uint16_t addr);

// Verify the mbuf matches 6502 memory.
void ria_verify_mbuf(uint16_t addr);

// The RIA is active when it's performing an mbuf action.
bool ria_active();

// Prints a "?" error and returns true if last mbuf action failed.
bool ria_print_error_message();

#endif /* _RIA_H_ */
