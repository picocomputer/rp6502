/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_CPU_H_
#define _HOST_POCKET_SW_CPU_H_

#include "core/cpu.h"

/* This machine's 6502 driver row. */
void cpu_init(void);
bool cpu_check_phi2_khz(uint16_t *v);
void cpu_apply_phi2_khz(uint16_t phi2_khz, bool changed);
void cpu_run(void);
void cpu_stop(void);

/* This driver's row in a machine's driver list; see core/driver.h. A row lives with the
 * implementation, not the contract, which is why no #undef is needed: this
 * is the only cpu row a Pocket translation unit can see. */
#define CPU_CONFIG_PHI2 CONFIG_INT(P, cpu, phi2_khz, uint16_t, CPU_PHI2_DEFAULT, \
    cpu_check_phi2_khz, cpu_apply_phi2_khz, STR_PHI2, cpu_phi2_response, \
    STR_HELP_SET_PHI2, NULL)
#define CPU_DRIVER DRIVER(cpu_init, nul_task, nul_task, cpu_run, cpu_stop, nul_break, \
    CPU_CONFIG_PHI2, nul_config)

#endif /* _HOST_POCKET_SW_CPU_H_ */
