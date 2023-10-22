/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CPU_H_
#define _CPU_H_

#include <stdint.h>
#include <stdbool.h>

#define CPU_RESB_PIN 26
#define CPU_IRQB_PIN 22
#define CPU_PHI2_PIN 21

// Short circuit this to the RIA action loop
extern volatile int cpu_rx_char;

// Kernel events
void cpu_init();
void cpu_task();
void cpu_run();
void cpu_stop();
void cpu_reclock();
void cpu_api_phi2();

// The CPU is active when RESB is high or when
// we're waiting for the RESB timer.
bool cpu_active();

// Config handlers
uint32_t cpu_validate_phi2_khz(uint32_t freq_khz);
bool cpu_set_phi2_khz(uint32_t freq_khz);

// Return calculated reset time. May be higher than requested
// to guarantee the 6502 gets two clock cycles during reset.
uint32_t cpu_get_reset_us();

// Receive UART and keyboard communications intended for the 6502.
void cpu_com_rx(uint8_t ch);

// Get char from CPU rx buf
int cpu_getchar(void);

// Readline support for stdin
void cpu_stdin_request(void);
bool cpu_stdin_ready(void);
size_t cpu_stdin_read(uint8_t *buf, size_t count);

#endif /* _CPU_H_ */
