/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for vga/sys/com.h and ria/sys/com.h. The console-input multiplexer
 * the vendored rln.c/term.c reach lives in the emulator's own com module header;
 * this shim just forwards the firmware path "sys/com.h" to it so the vendored
 * code compiles unchanged.
 */

#ifndef _EMU_SHIM_SYS_COM_H_
#define _EMU_SHIM_SYS_COM_H_

#include "emu/sys/com.h"

#endif /* _EMU_SHIM_SYS_COM_H_ */
