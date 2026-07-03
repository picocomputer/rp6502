/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for ria/str/str.h. The vendored std.c needs only the console
 * device names; the emulator defines them in api/std_drivers.c rather than
 * compiling str.c, which is bound to the firmware's cfg/cpu/printf stack.
 */

#ifndef _EMU_SHIM_STR_STR_H_
#define _EMU_SHIM_STR_STR_H_

extern const char STR_CON_COLON[];
extern const char STR_TTY_COLON[];

#endif /* _EMU_SHIM_STR_STR_H_ */
