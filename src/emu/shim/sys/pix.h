/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shadow for ria/sys/pix.h (whose PIO code can't build in the emu). A firmware
 * "sys/pix.h" include (e.g. ria/api/std.c's XRAM mirror) resolves here and gets
 * the emu's real PIX implementation, where the bus-side sends are no-ops because
 * its VGA renders from the same shared xram[].
 */

#ifndef _EMU_SHIM_SYS_PIX_H_
#define _EMU_SHIM_SYS_PIX_H_

#include "emu/sys/pix.h"

#endif /* _EMU_SHIM_SYS_PIX_H_ */
