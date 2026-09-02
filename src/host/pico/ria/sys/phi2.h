/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_PHI2_H_
#define _RIA_SYS_PHI2_H_

/* PHI2 on the board: the RIA's write state machine side-sets the pin, so
 * this owns the divider that machine runs at, and the fan-out to everything
 * else clocked from it. */

#include "core/wdc/phi2.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void phi2_init(void);

/* The rate this divider can actually land on, nearest the one asked for.
 * Pure arithmetic -- no hardware is read -- which is what lets the config
 * check normalize a value without reclocking anything. */
uint16_t phi2_quantize_khz(uint16_t freq_khz);

/* The configuration row's columns. */
bool phi2_check_khz(uint16_t *v);
void phi2_apply_khz(uint16_t phi2_khz, bool changed);
int phi2_response(char *buf, size_t buf_size, int state, unsigned width);

/* This driver's row in a machine's driver list; see core/driver.h. It must
 * come up after RIA and PIX, whose inits create the state machines a reclock
 * reprograms. */
#define PHI2_CONFIG_KHZ CONFIG_INT(P, phi2, khz, uint16_t, PHI2_DEFAULT_KHZ, \
    phi2_check_khz, phi2_apply_khz, STR_PHI2, phi2_response, \
    STR_HELP_SET_PHI2, NULL)
#define PHI2_DRIVER DRIVER(phi2_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    PHI2_CONFIG_KHZ, nul_config)

#endif /* _RIA_SYS_PHI2_H_ */
