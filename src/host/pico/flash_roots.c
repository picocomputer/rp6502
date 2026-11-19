/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The names of the TASK_ALWAYS tasks, the ones that run while the RIA
 * rewrites its own flash, for flash_check.py, which walks the code from them.
 * They are written into the ELF's .flash_roots section, as the driver table
 * has them in this build. The section is not loaded, so no byte of it reaches
 * flash or RAM. */

#include "drivers.h"

#define TASK_ALWAYS(f) \
    __asm__(".pushsection .flash_roots,\"\",%progbits\n.asciz \"" #f "\"\n.popsection");
#include "core/sys/driver_walk.h"
DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
