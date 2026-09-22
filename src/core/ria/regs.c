/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/ria/regs.h"
#include "machine.h"
#include <stdalign.h>

alignas(0x20) volatile uint8_t HOST_UNINITIALIZED_RAM(regs)[0x20];

uint8_t xstack[XSTACK_SIZE + 1];
volatile size_t xstack_ptr = XSTACK_SIZE;
