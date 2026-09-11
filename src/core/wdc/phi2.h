/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The kilohertz range is here rather than with a machine because every
 * machine's phi2 clamps to it, and core/str/str.c stringizes it into the SET
 * help line.
 */

#ifndef _CORE_WDC_PHI2_H_
#define _CORE_WDC_PHI2_H_

#include "core/sys/sst.h"
#include <stdbool.h>
#include <stdint.h>

#define PHI2_MIN_KHZ 100
#define PHI2_MAX_KHZ 8000
#define PHI2_DEFAULT_KHZ 8000

/* The rate actually running, which is not always the rate asked for, because
 * a machine whose clock divider cannot land on a whole kilohertz reports the
 * one it can. The configured rate is the separate phi2_get_khz/phi2_set_khz
 * pair that CONFIG_INT generates. */
uint16_t phi2_get_khz_run(void);
void phi2_set_khz_run(uint16_t phi2_khz);

void phi2_init(void);
bool phi2_check_khz(uint16_t *v);
void phi2_apply_khz(uint16_t phi2_khz, bool changed);

/* phi2_response is declared only on the machines whose monitor asks for it.
 * Everywhere else it names nothing, which compiles because CONFIG_INT discards
 * the tail arguments it sits in. */
#define PHI2_CONFIG_KHZ CONFIG_INT(P, phi2, khz, uint16_t, PHI2_DEFAULT_KHZ, \
    phi2_check_khz, phi2_apply_khz, STR_PHI2, phi2_response, STR_HELP_SET_PHI2, NULL)
/* A savestate carries the rate the machine is running at, not the rate it is
 * configured for: a ROM may set its own, and a reset takes it back to the
 * configured one. */
#define PHI2_SST_SIZE 2
void phi2_sst_save(sst_cursor_t *c, unsigned flags);
bool phi2_sst_load(sst_cursor_t *c, unsigned flags);

#define PHI2_DRIVER DRIVER(phi2_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    PHI2_CONFIG_KHZ, nul_config, SST(PHI2, 1, PHI2_SST_SIZE, phi2_sst_save, phi2_sst_load))

#endif /* _CORE_WDC_PHI2_H_ */
