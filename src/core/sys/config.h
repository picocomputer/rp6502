/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The machine's persisted settings, assembled from its driver roster.
 *
 * A driver names a setting in the config columns of its DRIVER row and
 * contributes two functions: a pure check that judges a value and says what
 * gets stored, and an apply that makes the machine match. Everything else --
 * the byte itself, the accessor pair, the file's parse and render, and the
 * decision to save -- is generated here from the same row.
 *
 * The accessors keep the driver's own names, so a caller cannot tell a
 * generated setter from a hand-written one, and nothing outside this file
 * can reach the byte.
 *
 * A machine that has not got a driver has not got its settings: the row is
 * absent from the roster and never expands. That is the only gate there is.
 */

#ifndef _CORE_SYS_CONFIG_H_
#define _CORE_SYS_CONFIG_H_

#include "drivers.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One accessor pair per row this machine carries. */
#define DRIVER(i, t, iot, r, s, b, c1, c2) c1 c2
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

/* One "+" line, letter and value already split off. Runs the row's check so
 * a value the machine cannot hold leaves the default standing; never applies,
 * so a file can be read before the drivers that answer for it are up. */
void config_load_line(char letter, const char *value);

/* Every row, rendered as the file holds it. The store supplies the sink --
 * on a machine that compares before writing, the same call serves both. */
typedef void (*config_sink_t)(char c, void *arg);
void config_render(config_sink_t sink, void *arg);

#endif /* _CORE_SYS_CONFIG_H_ */
