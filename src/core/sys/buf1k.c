/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/buf1k.h"
#include <stdalign.h>

alignas(4) uint8_t buf1k[BUF1K_SIZE];
