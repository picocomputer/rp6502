/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/vol.h"
#include "core/aud/mix.h"
#include "core/hid/usage.h"
#include "core/str/str.h"
#include "core/sys/config.h"
#include "host/host.h"
#include "machine.h"
#include <stdio.h>

/* The keys reach this from keyboard_on_press, which runs while flashing, so
 * the table and the functions to vol_key are in RAM. */
const uint16_t HOST_IN_RAM vol_gain[1 - VOL_DB_MUTE] = {
    32768, 29205, 26029, 23198, 20675, 18427, 16423, 14637,
    13045, 11627, 10362, 9235, 8231, 7336, 6538, 5827,
    5193, 4629, 4125, 3677, 3277, 2920, 2603, 2320,
    2068, 1843, 1642, 1464, 1305, 1163, 1036, 924,
    823, 734, 654, 583, 519, 463, 413, 368,
    328, 292, 260, 232, 207, 184, 164, 146,
    130, 116, 104, 92, 82, 73, 65, 58,
    52, 46, 41, 37, 33, 29, 26, 23,
    0};

/* The program's, from its xreg. */
static int16_t vol_prog_db, vol_prog_bal;

/* The user's volume as it plays: SET VOL's, or where the keys have moved it
 * ahead of the save at vol_save_at. */
static int8_t vol_live_db;
static int8_t vol_unmute_db;
static uint64_t vol_save_at;

static int HOST_IN_RAM_FUNC vol_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static uint16_t HOST_IN_RAM_FUNC vol_q15(int db)
{
    return db <= VOL_DB_MUTE ? 0 : vol_gain[-db];
}

static void HOST_IN_RAM_FUNC vol_update(void)
{
    const int user = vol_live_db;
    const int ubal = vol_get_balance();
    const int eng = vol_clamp(user + vol_prog_db, VOL_DB_MUTE, 0);
    const int bal = vol_clamp(ubal + vol_prog_bal, -VOL_BAL_MAX, VOL_BAL_MAX);
    aud_set_gain(vol_q15(eng - (bal > 0 ? bal : 0)),
                 vol_q15(eng + (bal < 0 ? bal : 0)),
                 vol_q15(user - (ubal > 0 ? ubal : 0)),
                 vol_q15(user + (ubal < 0 ? ubal : 0)));
}

static bool vol_sst_loaded(sst_cursor_t *c)
{
    if (vol_prog_db > 0 || vol_prog_db < VOL_DB_MUTE)
        return sst_refuse(c, "volume out of range");
    if (vol_prog_bal > VOL_BAL_MAX || vol_prog_bal < -VOL_BAL_MAX)
        return sst_refuse(c, "balance out of range");
    vol_update();
    return true;
}

#define VOL_SST(X)                                              \
    X(I16, vol_prog_db)                                         \
    X(I16, vol_prog_bal)                                        \
    X(VALIDATE, vol_sst_loaded)                                 \
    X(SKIP, vol_live_db, "the user's, not the machine's")       \
    X(SKIP, vol_unmute_db, "the user's, not the machine's")     \
    X(SKIP, vol_save_at, "the user's, not the machine's")
SST_TABLE(vol, VOL_SST)

void vol_init(void)
{
    vol_prog_db = vol_prog_bal = 0;
    vol_live_db = vol_get_volume();
    vol_unmute_db = vol_live_db > VOL_DB_MUTE ? vol_live_db : 0;
    vol_save_at = 0;
    vol_update();
}

void vol_stop(void)
{
    vol_prog_db = vol_prog_bal = 0;
    vol_update();
}

void vol_task(void)
{
    if (vol_save_at && (int64_t)(host_clock_us() - vol_save_at) >= 0)
    {
        vol_save_at = 0;
        vol_set_volume(vol_live_db);
    }
}

void HOST_IN_RAM_FUNC vol_key(uint8_t keycode)
{
    int db = vol_live_db;
    switch (keycode)
    {
    case HID_KEY_VOLUME_UP:
        db = db <= VOL_DB_MUTE ? vol_unmute_db : db + VOL_KEY_STEP;
        break;
    case HID_KEY_VOLUME_DOWN:
        db -= VOL_KEY_STEP;
        break;
    case HID_KEY_MUTE:
        db = db <= VOL_DB_MUTE ? vol_unmute_db : VOL_DB_MUTE;
        break;
    default:
        return;
    }
    db = vol_clamp(db, VOL_DB_MUTE, 0);
    if (db > VOL_DB_MUTE)
        vol_unmute_db = (int8_t)db;
    vol_live_db = (int8_t)db;
    vol_update();
    vol_save_at = host_clock_us() + (uint64_t)VOL_KEY_SAVE_MS * 1000;
}

bool vol_xreg_volume(uint16_t word)
{
    const int16_t db = (int16_t)word;
    if (db > 0 || db < VOL_DB_MUTE)
        return false;
    vol_prog_db = db;
    vol_update();
    return true;
}

bool vol_xreg_balance(uint16_t word)
{
    const int16_t bal = (int16_t)word;
    if (bal > VOL_BAL_MAX || bal < -VOL_BAL_MAX)
        return false;
    vol_prog_bal = bal;
    vol_update();
    return true;
}

bool vol_check_volume(int8_t *v)
{
    return *v <= 0 && *v >= VOL_DB_MUTE;
}

void vol_apply_volume(int8_t db, bool changed)
{
    (void)changed;
    vol_live_db = db;
    if (db > VOL_DB_MUTE)
        vol_unmute_db = db;
    vol_save_at = 0;
    vol_update();
}

int vol_volume_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)state;
    (void)width;
    snprintf(buf, buf_size, STR_SET_VOL_RESPONSE, vol_live_db,
             vol_live_db <= VOL_DB_MUTE ? STR_VOL_MUTE : "");
    return -1;
}

bool vol_check_balance(int8_t *v)
{
    return *v <= VOL_BAL_MAX && *v >= -VOL_BAL_MAX;
}

void vol_apply_balance(int8_t bal, bool changed)
{
    (void)bal;
    (void)changed;
    vol_update();
}

int vol_balance_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)state;
    (void)width;
    snprintf(buf, buf_size, STR_SET_BAL_RESPONSE, vol_get_balance());
    return -1;
}
