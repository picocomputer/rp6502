/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for ria/sys/mem.h — now contract-only (the mbuf engine lives in
 * sys/mem_hw.h, never compiled here). The emulator substitutes its own
 * representations of that contract (regs[], ram[], xram[], xstack[],
 * xstack_ptr, REGS/REGSW in emu/sys/mem.h, and the API_* register aliases in
 * emu/api/api.h), so ria/api/api.h and the vendored aud drivers compile
 * unchanged against the emulator's state.
 */

#ifndef _EMU_SHIM_SYS_MEM_H_
#define _EMU_SHIM_SYS_MEM_H_

#include "emu/sys/mem.h"
#include "emu/api/api.h"

#endif /* _EMU_SHIM_SYS_MEM_H_ */
