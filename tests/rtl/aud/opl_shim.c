/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What opl.c takes from the rest of core: the mapping. */

#include "core/aud/mix.h"
#include "core/aud/opl.h"

void aud_opl_map(uint16_t word) { opl_map(word); }
