/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_AUD_H_
#define _FPGA_SW_AUD_H_

#include <stdbool.h>
#include <stdint.h>

void aud_init(void);
void aud_stop(void);
/* The deferred halves of the restore's log, taken after the machine has
 * been running again for a while. */
void aud_task(void);
/* A savestate carries the blocks and the pointers but not what the
 * engines made of them; this is the pointers put back and the blocks
 * replayed. */
void aud_restore(void);
bool aud_psg_xreg(uint16_t word);
bool aud_opl_xreg(uint16_t word);

#endif /* _FPGA_SW_AUD_H_ */
