/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_VOL_H_
#define _CORE_AUD_VOL_H_

/* The whole output's volume and balance, in dB.
 *
 * The user sets them with SET VOL and SET BAL, which the config file keeps,
 * and with a keyboard's Volume Up, Volume Down and Mute. A program sets its
 * own with the RIA's xreg, which a stop puts back to 0, and a savestate
 * keeps. The two add, so a program can only make the sound quieter than the
 * user has it. The bell is the user's alone: a program's volume and balance
 * leave it be.
 *
 * Volume runs from 0 down to -63 dB; VOL_DB_MUTE and below is silence.
 * Balance runs from -63 to 63: +n takes n dB off the left channel and -n
 * off the right. Each mixer is handed the resulting Q15 gains through
 * aud_set_gain.
 */

#include "core/sys/sst.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define VOL_DB_MUTE (-64)
#define VOL_BAL_MAX 63

/* Q15 gain for 0 to VOL_DB_MUTE dB, by attenuation: round(32768 *
 * 10^(-n/20)), and 0 at the mute. */
extern const uint16_t vol_gain[1 - VOL_DB_MUTE];

/* The xreg words, as int16: volume 0 to VOL_DB_MUTE, balance -VOL_BAL_MAX
 * to VOL_BAL_MAX. Anything else is refused and changes nothing. */
bool vol_xreg_volume(uint16_t word);
bool vol_xreg_balance(uint16_t word);

/* A key pressed on a keyboard: Volume Up and Down step the user's volume by
 * VOL_KEY_STEP dB and Mute silences it or gives it back. Any other key is
 * ignored. The new volume plays at once and is saved once the keys have
 * rested for VOL_KEY_SAVE_MS. */
#define VOL_KEY_STEP 3
#define VOL_KEY_SAVE_MS 2000
void vol_key(uint8_t keycode);

void vol_init(void);
void vol_task(void);
void vol_stop(void);

bool vol_check_volume(int8_t *v);
void vol_apply_volume(int8_t db, bool changed);
int vol_volume_response(char *buf, size_t buf_size, int state, unsigned width);
bool vol_check_balance(int8_t *v);
void vol_apply_balance(int8_t bal, bool changed);
int vol_balance_response(char *buf, size_t buf_size, int state, unsigned width);

extern const sst_table_t vol_sst;

#define VOL_CONFIG_VOLUME CONFIG_INT(G, vol, volume, int8_t, 0,       \
    vol_check_volume, vol_apply_volume, STR_VOL, vol_volume_response, \
    STR_HELP_SET_VOL, NULL)
#define VOL_CONFIG_BALANCE CONFIG_INT(Q, vol, balance, int8_t, 0,         \
    vol_check_balance, vol_apply_balance, STR_BAL, vol_balance_response, \
    STR_HELP_SET_BAL, NULL)
#define VOL_DRIVER DRIVER(                                  \
    INIT(vol_init)                                          \
    TASK_FATFS(vol_task)                                    \
    STOP(vol_stop)                                          \
    RESET(vol_stop)                                         \
    VOL_CONFIG_VOLUME                                       \
    VOL_CONFIG_BALANCE                                      \
    SST("VOL_", vol))

#endif /* _CORE_AUD_VOL_H_ */
