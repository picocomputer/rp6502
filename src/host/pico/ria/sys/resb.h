/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_RESB_H_
#define _RIA_SYS_RESB_H_

#include "core/wdc/resb.h"
#include <stdint.h>

#define CPU_RESB_PIN 26

uint32_t resb_get_reset_us(void);

void resb_task(void);

#define RESB_DRIVER DRIVER(nul_init, resb_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, nul_sst)

#endif /* _RIA_SYS_RESB_H_ */
