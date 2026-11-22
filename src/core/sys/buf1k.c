/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/buf1k.h"
#include "core/sys/sst.h"
#include <stdalign.h>

alignas(4) uint8_t buf1k[BUF1K_SIZE];

#define BUF1K_SST(X) X(SKIP, buf1k, "scratch, dead at every frame boundary")
SST_TABLE(buf1k, BUF1K_SST)
