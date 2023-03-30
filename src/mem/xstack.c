/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "xstack.h"

// The xstack is:
// 256 bytes, enough to hold a CC65 stack frame
// 1 byte at end always zero for cstrings

// Many OS calls can use xstack instead of xram for cstrings.
// Using xstack doesn't require sending the zero termination.
// Cstrings and data are pushed in reverse so data is ordered correctly on a the top down stack.
// Cstrings and data are pulled in reverse to expedite use of returned y resister
// TODO can we use mbuf for this? it's big enough to reverse a 256 byte return.

uint8_t xstack[XSTACK_SIZE + 1];
size_t volatile xstack_ptr;
