/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* A machine of the VCP and the NFC reader alone, for test_vcp.c. */

#ifndef _HOST_DRIVERS_H_
#define _HOST_DRIVERS_H_
#include "core/sys/driver.h"
#include "ria/usb/vcp.h"
#include "ria/usb/nfc.h"
#define RP6502_MACH_DRIVERS VCP_DRIVER, NFC_DRIVER
#endif
