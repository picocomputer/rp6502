/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for ria/sys/mem.h. The firmware header declares the RIA register
 * window, the shared XRAM, and the xstack; the emulator already owns all of
 * these (regs[], ram[], xram[], xstack[], xstack_ptr, REGS/REGSW in
 * emu/sys/mem.h, and the API_* register aliases in emu/api/api.h). Forwarding
 * the firmware's "sys/mem.h" to those lets ria/api/api.h (and the vendored
 * aud drivers) compile unchanged against the emulator's state.
 */

#ifndef _EMU_SHIM_SYS_MEM_H_
#define _EMU_SHIM_SYS_MEM_H_

#include "emu/sys/mem.h"
#include "emu/api/api.h"

#endif /* _EMU_SHIM_SYS_MEM_H_ */
