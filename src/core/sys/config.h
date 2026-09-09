/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The machine's persisted settings, assembled from its driver roster. A driver
 * names a setting in the config columns of its DRIVER row and contributes a
 * check that judges a value and an apply that makes the machine match. This
 * header expands the row into the accessor pair; core/sys/config.c expands the
 * same row into the storage, the accessor bodies, and the file's parse and
 * render. A machine that has not got the driver never expands the row and so
 * has not got the setting.
 */

#ifndef _CORE_SYS_CONFIG_H_
#define _CORE_SYS_CONFIG_H_

#include "drivers.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DRIVER(i, t, iot, r, s, b, c1, c2, ...) c1 c2
#define CONFIG_INT(ltr, pfx, name, type, def, check, apply, ...) \
    type pfx##_get_##name(void);                                 \
    bool pfx##_set_##name(type v);
#define CONFIG_STR(ltr, pfx, name, size, def, check, apply, ...) \
    const char *pfx##_get_##name(void);                          \
    bool pfx##_set_##name(const char *v);
#define CONFIG_RAW CONFIG_STR
#define CONFIG_HIDDEN(l, p, n, sz, d, c, a) CONFIG_STR(l, p, n, sz, d, c, a, 0, 0, 0, 0)
#define CONFIG_SAVE(fn)
DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef CONFIG_SAVE
#undef CONFIG_HIDDEN
#undef CONFIG_RAW
#undef CONFIG_STR
#undef CONFIG_INT
#undef DRIVER

/* One "+" line, letter and value already split off. The row's check runs, so a
 * value this machine cannot hold leaves the default standing, but the row's
 * apply never does, so the file can be read before the drivers that answer for
 * it are up. */
void config_load_line(char letter, const char *value);

typedef void (*config_sink_t)(char c, void *arg);
void config_render(config_sink_t sink, void *arg);

#endif /* _CORE_SYS_CONFIG_H_ */
