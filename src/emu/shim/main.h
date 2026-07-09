/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for ria/main.h. The shared api.c reaches main_api() through the
 * firmware path "main.h", which would otherwise resolve to the VGA tree's
 * main.h (src/vga precedes src/ria on the include path). The emulator's
 * main_api (the op registry) lives in emu/main.c.
 */

#ifndef _EMU_SHIM_MAIN_H_
#define _EMU_SHIM_MAIN_H_

#include "ria/main.h"

#endif /* _EMU_SHIM_MAIN_H_ */
