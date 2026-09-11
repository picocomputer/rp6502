/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/bel.h"
#include "machine.h"

HOST_IN_FLASH("bel_teletype") const ria_bel_t bel_teletype = {
    .freq = 1760,
    .duty = 215,          // gate open for 214 of 256 phase steps
    .vol_attack = 0x51,   // attack to -5vol, 8ms full-swing rate
    .vol_decay = 0x60,    // decay to -6vol, 6ms full-swing rate
    .wave_release = 0x39, // triangle, 750ms full-swing release rate
    .restrike_ms = 100,   // restrike at 10 Hz
    .release_ms = 20,
    .end_ms = 800,
};

HOST_IN_FLASH("bel_nfc_fail") const ria_bel_t bel_nfc_fail = {
    .freq = 330,
    .duty = 127,          // 50% square
    .vol_attack = 0x80,   // attack to -8vol, 2ms full-swing rate
    .vol_decay = 0x80,    // sustain at -8vol
    .wave_release = 0x15, // square, 168ms full-swing release rate
    .restrike_ms = 0,
    .release_ms = 200,
    .end_ms = 420,
};

HOST_IN_FLASH("bel_nfc_success_1") const ria_bel_t bel_nfc_success_1 = {
    .freq = 784,
    .duty = 255,          // gate open for 254 of 256 phase steps
    .vol_attack = 0x60,   // attack to -6vol, 2ms full-swing rate
    .vol_decay = 0x60,    // sustain at -6vol
    .wave_release = 0x03, // sine, 72ms full-swing release rate
    .restrike_ms = 0,
    .release_ms = 90,
    .end_ms = 170,
};

HOST_IN_FLASH("bel_nfc_success_2") const ria_bel_t bel_nfc_success_2 = {
    .freq = 1568,
    .duty = 255,          // gate open for 254 of 256 phase steps
    .vol_attack = 0x60,   // attack to -6vol, 2ms full-swing rate
    .vol_decay = 0x60,    // sustain at -6vol
    .wave_release = 0x06, // sine, 204ms full-swing release rate
    .restrike_ms = 0,
    .release_ms = 130,
    .end_ms = 350,
};
