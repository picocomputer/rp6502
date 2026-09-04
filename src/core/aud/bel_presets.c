/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The sounds the machine can ring, and nothing else. They are data, not
 * code, and every host that has a bell wants the same ones — the RP2350
 * and the emulator run them through bel.c's synth, the Pocket hands them
 * to a voice in the fabric. One definition either way.
 */

#include "core/aud/bel.h"
#include "machine.h"

// Teletype bell: restrike-capable
HOST_IN_FLASH("bel_teletype") const ria_bel_t bel_teletype = {
    .freq = 1760,
    .duty = 215,          // hint of grit
    .vol_attack = 0x51,   // attack to -5vol in 8ms
    .vol_decay = 0x60,    // decay to -6vol in 6ms
    .wave_release = 0x39, // triangle wave, release to zero in 750ms
    .restrike_ms = 100,   // restrike 10 Hz
    .release_ms = 20,
    .end_ms = 800,
};

// NFC fail/error: low square buzz
HOST_IN_FLASH("bel_nfc_fail") const ria_bel_t bel_nfc_fail = {
    .freq = 330,
    .duty = 127,          // 50% square
    .vol_attack = 0x80,   // attack to -8vol in 2ms
    .vol_decay = 0x80,    // sustain at -8vol
    .wave_release = 0x15, // square, release to zero in 168ms
    .restrike_ms = 0,
    .release_ms = 200,
    .end_ms = 420,
};

// NFC success note 1
HOST_IN_FLASH("bel_nfc_success_1") const ria_bel_t bel_nfc_success_1 = {
    .freq = 784,
    .duty = 255,          // full cycle
    .vol_attack = 0x60,   // attack to -6vol in 2ms
    .vol_decay = 0x60,    // sustain at -6vol
    .wave_release = 0x03, // sine, release to zero in 72ms
    .restrike_ms = 0,
    .release_ms = 90,
    .end_ms = 170,
};

// NFC success note 2
HOST_IN_FLASH("bel_nfc_success_2") const ria_bel_t bel_nfc_success_2 = {
    .freq = 1568,
    .duty = 255,          // full cycle
    .vol_attack = 0x60,   // attack to -6vol in 2ms
    .vol_decay = 0x60,    // sustain at -6vol
    .wave_release = 0x06, // sine, release to zero in 204ms
    .restrike_ms = 0,
    .release_ms = 130,
    .end_ms = 350,
};
