/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vstack.h"

uint8_t volatile vstack[VSTACK_SIZE];
size_t volatile vstack_ptr;
