/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vstack.h"

// The vstack is:
// 256 bytes, enough to hold a CC65 stack frame
// 2 bytes for fastcall sreg
// 2 bytes for alignment
// 1 byte at end always zero for cstrings

// Many OS calls can use vstack instead of vram for cstrings.
// Using vstack doesn't require sending the zero termination.
// Cstrings must be pushed in reverse (top down stack).

uint8_t vstack[VSTACK_SIZE + 1];
size_t volatile vstack_ptr;
