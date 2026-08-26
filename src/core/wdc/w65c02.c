/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6502 as software, beside the same part as fabric in w65c02.sv. This
 * emits the vendored model and nothing else: what a machine wires around it
 * -- the PHI2 divider, the halt gate, the tick the bus is clocked from -- is
 * core/wdc/cpu.c, the way via.c's own wiring sits beside the m6522.
 */

#define CHIPS_IMPL
#include "chips/chips/w65c02.h"
