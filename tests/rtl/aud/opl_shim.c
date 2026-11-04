/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What opl.c takes from the rest of core: XRAM and the engine lock. */

#include "core/aud/mix.h"
#include "core/sys/xram.h"

static uint8_t shim_xram[0x10000];
volatile uint8_t *const xram = shim_xram;

void aud_engine_lock(void) {}
void aud_engine_unlock(void) {}
