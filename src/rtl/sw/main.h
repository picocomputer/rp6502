/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_MAIN_H_
#define _FPGA_SW_MAIN_H_

/* The restage triggers re-synced after a restore, so a wake's fresh
 * host announcements do not read as a new program. */
void main_restored(void);

#include <stdint.h>
#include <stdbool.h>

/* The xreg fan-outs, the emulator's shape: device 0 is the RIA-local
 * virtual xreg, device 1 the video device on this fabric. */
bool main_xreg_0(uint8_t channel, uint8_t address, uint16_t word);
bool main_xreg_1(uint8_t channel, uint8_t address, uint16_t word);

/* This platform's time zone is the menu's UTC offset in signed
 * minutes, not a POSIX string; time.c turns it into one for the C
 * library. cfg_task feeds changes here. */
void tim_set_tz_minutes(int32_t min);

#endif /* _FPGA_SW_MAIN_H_ */
