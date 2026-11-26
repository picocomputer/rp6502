/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* A machine of PIX and the VGA's link alone, for test_vga.c. */

#ifndef _HOST_DRIVERS_H_
#define _HOST_DRIVERS_H_
#include "core/sys/driver.h"
#include "ria/sys/pix.h"
#include "ria/sys/vga.h"
#define RP6502_MACH_DRIVERS PIX_DRIVER, VGA_DRIVER
#endif
