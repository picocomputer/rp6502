/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "emu/sys/mem.h"
#include "emu/sys/sys.h"

uint8_t ram[0x10000];
uint8_t xram[0x10000];

volatile uint8_t regs[32];

uint8_t xstack[XSTACK_SIZE + 1];
size_t xstack_ptr = XSTACK_SIZE;

/* XRAM write-notify ring (ria/sys/mem.c): windowed writes to the active audio
 * device's page are recorded here for its sample handler to drain. */
volatile uint8_t xram_queue_page;
volatile uint8_t xram_queue_head;
volatile uint8_t xram_queue_tail;
volatile uint8_t xram_queue[256][2];

bool emu_cpu_halted = false;
int emu_exit_code = 0;
