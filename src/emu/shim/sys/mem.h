/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for ria/sys/mem.h. Memory is deliberately per-platform (the
 * firmware header is never compiled here); the emulator substitutes its own
 * representations (regs[], ram[], xram[], xstack[], xstack_ptr, REGS/REGSW
 * in emu/sys/mem.h), so ria/api/api.h and the vendored aud drivers compile
 * unchanged against the emulator's state.
 */

#ifndef _EMU_SHIM_SYS_MEM_H_
#define _EMU_SHIM_SYS_MEM_H_

#include "emu/sys/mem.h"

#endif /* _EMU_SHIM_SYS_MEM_H_ */
