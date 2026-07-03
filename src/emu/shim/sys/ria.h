/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for ria/sys/ria.h. The RIA-side ABI the vendored rln.c/atr.c reach
 * (ria_active / ria_trigger_sigint / ria_get_sigint) lives in the emulator's own
 * RIA chip header; this shim just forwards the firmware path "sys/ria.h" to it
 * so the vendored code compiles unchanged.
 */

#ifndef _EMU_SHIM_SYS_RIA_H_
#define _EMU_SHIM_SYS_RIA_H_

#include "emu/chips/rp6502.h"

#endif /* _EMU_SHIM_SYS_RIA_H_ */
