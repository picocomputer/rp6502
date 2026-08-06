/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vga/sys/mem.h"
#include <pico.h>
#include <stdalign.h>

// 4KB segments because a single 64KB array crashes my debugger
alignas(0x10000) static uint8_t __uninitialized_ram(xram_blocks)[16][0x1000];
volatile uint8_t *const xram = (uint8_t *)xram_blocks;
