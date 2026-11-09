/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Opens a walk of the driver list, so it has no include guard. Every label
 * the walk did not define expands to nothing, and takes the number of
 * arguments its real form takes, so a malformed row fails to compile.
 */

#ifdef DRIVER_WALK_OPEN
#error "a driver walk is already open"
#endif
#define DRIVER_WALK_OPEN

#ifndef DRIVER
#define DRIVER(row) row
#endif
#ifndef INIT
#define INIT(f)
#endif
#ifndef TASK_FATFS
#define TASK_FATFS(f)
#endif
#ifndef TASK_PUMPED
#define TASK_PUMPED(f)
#endif
#ifndef RUN
#define RUN(f)
#endif
#ifndef STOP
#define STOP(f)
#endif
#ifndef BREAK
#define BREAK(f)
#endif
#ifndef CONFIG_INT
#define CONFIG_INT(ltr, pfx, name, type, def, check, apply, attr, resp, help, helpfn)
#endif
#ifndef CONFIG_STR
#define CONFIG_STR(ltr, pfx, name, size, def, check, apply, attr, resp, help, helpfn)
#endif
#ifndef CONFIG_RAW
#define CONFIG_RAW(ltr, pfx, name, size, def, check, apply, attr, resp, help, helpfn)
#endif
#ifndef CONFIG_HIDDEN
#define CONFIG_HIDDEN(ltr, pfx, name, size, def, check, apply)
#endif
#ifndef CONFIG_SAVE
#define CONFIG_SAVE(fn)
#endif
#ifndef SST
#define SST(id, ver, size, save, load)
#endif
