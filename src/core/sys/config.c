/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/config.h"
#include "core/str/str.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The file's shape, not any one setting's. Bumped only if a reader would
 * have to behave differently -- a row appearing or leaving does not. */
#define CONFIG_VERSION 1

/* Long enough for the widest row plus its letter and newline; the NFC device
 * hash is the one that sets it. */
#define CONFIG_LINE_MAX 160

/* A set reached from inside another set -- an apply that clears a companion
 * row -- must not write the file twice. The outermost one writes. */
static uint8_t config_depth;
static bool config_dirty;

/* Letter uniqueness, for free: two rows sharing one costs a redeclared
 * enumerator. The name collides in the storage below for the same reason.
 * The letters C and R are retired -- they were Caps and RESB. Do not reuse. */
#define DRIVER(i, t, iot, r, s, b, c1, c2) c1 c2
#define CONFIG_INT(ltr, pfx, name, type, def, check, apply, ...) \
    enum { config_letter_##ltr };
#define CONFIG_STR(ltr, pfx, name, size, def, check, apply, ...) \
    enum { config_letter_##ltr };
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

/* The bytes. Nothing else in the machine defines one, and the compile-time
 * default is what a host that sets before sys_init writes over. */
#define DRIVER(i, t, iot, r, s, b, c1, c2) c1 c2
#define CONFIG_INT(ltr, pfx, name, type, def, check, apply, ...) \
    static type config_##pfx##_##name = def;
#define CONFIG_STR(ltr, pfx, name, size, def, check, apply, ...) \
    static char config_##pfx##_##name[size] = def;
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

/* The store, if this machine has one. A machine with no CONFIG_SAVE row
 * still validates, stores and applies; only the write is absent. */
static void config_save_now(void)
{
#define DRIVER(i, t, iot, r, s, b, c1, c2) c1 c2
#define CONFIG_INT(...)
#define CONFIG_STR(...)
#define CONFIG_RAW(...)
#define CONFIG_HIDDEN(...)
#define CONFIG_SAVE(fn) fn();
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef CONFIG_SAVE
#undef CONFIG_HIDDEN
#undef CONFIG_RAW
#undef CONFIG_STR
#undef CONFIG_INT
#undef DRIVER
}

static bool config_end(bool ok, bool changed)
{
    if (changed)
        config_dirty = true;
    if (!--config_depth && config_dirty)
    {
        config_dirty = false;
        config_save_now();
    }
    return ok;
}

/* check judges and normalizes, the store takes what it said, apply takes what
 * was asked -- so an action value reaches the driver intact -- and runs
 * whether or not the byte moved. */
#define DRIVER(i, t, iot, r, s, b, c1, c2) c1 c2
#define CONFIG_INT(ltr, pfx, name, type, def, check, apply, ...)  \
    type pfx##_get_##name(void) { return config_##pfx##_##name; } \
    bool pfx##_set_##name(type v)                                 \
    {                                                             \
        type stored = v;                                          \
        ++config_depth;                                           \
        if (!check(&stored))                                      \
            return config_end(false, false);                      \
        bool changed = config_##pfx##_##name != stored;           \
        config_##pfx##_##name = stored;                           \
        apply(v, changed);                                        \
        return config_end(true, changed);                         \
    }
#define CONFIG_STR(ltr, pfx, name, size, def, check, apply, ...)         \
    const char *pfx##_get_##name(void) { return config_##pfx##_##name; } \
    bool pfx##_set_##name(const char *v)                                 \
    {                                                                    \
        char stored[size];                                               \
        ++config_depth;                                                  \
        if (strlen(v) >= (size))                                         \
            return config_end(false, false);                             \
        strcpy(stored, v);                                               \
        if (!check(v, stored))                                           \
            return config_end(false, false);                             \
        bool changed = strcmp(config_##pfx##_##name, stored) != 0;       \
        strcpy(config_##pfx##_##name, stored);                           \
        apply(v, changed);                                               \
        return config_end(true, changed);                                \
    }
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

void config_load_line(char letter, const char *value)
{
/* An if-chain rather than a switch: stringizing a letter gives "P", and
 * "P"[0] is not an integer constant expression, so it cannot be a case
 * label. An unknown letter falls through -- a retired one, or a row this
 * build has not got -- which is what lets a file written by any machine
 * load on any other. */
#define DRIVER(i, t, iot, r, s, b, c1, c2) c1 c2
#define CONFIG_INT(ltr, pfx, name, type, def, check, apply, ...) \
    if (letter == (#ltr)[0])                                     \
    {                                                            \
        uint32_t parsed;                                         \
        if (str_parse_uint32(&value, &parsed) &&                 \
            parsed == (uint32_t)(type)parsed)                    \
        {                                                        \
            type stored = (type)parsed;                          \
            if (check(&stored))                                  \
                config_##pfx##_##name = stored;                  \
        }                                                        \
        return;                                                  \
    }
#define CONFIG_STR(ltr, pfx, name, size, def, check, apply, ...) \
    if (letter == (#ltr)[0])                                     \
    {                                                            \
        char stored[size];                                       \
        if (strlen(value) < (size))                              \
        {                                                        \
            strcpy(stored, value);                               \
            if (check(value, stored))                            \
                strcpy(config_##pfx##_##name, stored);           \
        }                                                        \
        return;                                                  \
    }
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
    (void)letter;
    (void)value;
}

static void config_emit(config_sink_t sink, void *arg, const char *fmt, ...)
{
    char buf[CONFIG_LINE_MAX];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    for (const char *p = buf; *p; p++)
        sink(*p, arg);
}

void config_render(config_sink_t sink, void *arg)
{
    config_emit(sink, arg, "+V%u\n", CONFIG_VERSION);
#define DRIVER(i, t, iot, r, s, b, c1, c2) c1 c2
#define CONFIG_INT(ltr, pfx, name, type, def, check, apply, ...) \
    config_emit(sink, arg, "+" #ltr "%u\n",                      \
                (unsigned)config_##pfx##_##name);
#define CONFIG_STR(ltr, pfx, name, size, def, check, apply, ...) \
    config_emit(sink, arg, "+" #ltr "%s\n", config_##pfx##_##name);
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
}
