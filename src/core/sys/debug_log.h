/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One macro: RP6502_LOG(category, LEVEL, fmt, ...). The category is a word
 * for what the message is about, the level ERROR, WARN, INFO or DEBUG, the
 * rest a printf.
 *
 *     RP6502_LOG(ntp, WARN, "no reply in %u ms", ms);
 *
 * What a build carries is decided when it is configured; a call above it
 * compiles to nothing, format string included:
 *
 *     cmake -B build                                   Release: nothing. Debug: ERROR.
 *     cmake -B build -DRP6502_LOG_LEVEL=DEBUG          everything
 *     cmake -B build -DRP6502_LOG_LEVELS="ntp=DEBUG;usb=INFO"
 *                                                      these categories, the rest as above
 *     cmake -B build -DRP6502_LOG_LEVELS="tinyusb=DEBUG"
 *                                                      a USB session on the Pico: the stack's
 *                                                      own output, and its LOG follows
 *
 * Where it lands is the machine's: host_log, which its host defines the way
 * it defines host/host.h, and tests/bench/tb_log.c answers for a test.
 */

#ifndef _CORE_SYS_DEBUG_LOG_H_
#define _CORE_SYS_DEBUG_LOG_H_

/* A Release build is NONE and says nothing. ERROR is what a Debug build says
 * by default: a failure nobody would otherwise hear of, used sparingly, and
 * with luck never needed to catch a problem in the field. */
#define RP6502_LOG_NONE 0
#define RP6502_LOG_ERROR 1
#define RP6502_LOG_WARN 2
#define RP6502_LOG_INFO 3
#define RP6502_LOG_DEBUG 4
#define RP6502_LOG_LEVEL_NAMES {"", "ERROR", "WARN", "INFO", "DEBUG"}
#ifndef RP6502_LOG_LEVEL
#error "RP6502_LOG_LEVEL"
#endif

/* A category's level is the build's word for it, RP6502_LOG_LEVEL_ntp, when
 * the build gave one, else RP6502_LOG_LEVEL. Told apart in the preprocessor:
 * a given level pastes onto a placeholder that expands to two arguments,
 * and an unset name pastes onto nothing, so the second argument is the
 * level or the default. */
#define RP6502_LOG_PH_0 0, 0
#define RP6502_LOG_PH_1 0, 1
#define RP6502_LOG_PH_2 0, 2
#define RP6502_LOG_PH_3 0, 3
#define RP6502_LOG_PH_4 0, 4
#define RP6502_LOG_JOIN_(a, b) a##b
#define RP6502_LOG_JOIN(a, b) RP6502_LOG_JOIN_(a, b)
#define RP6502_LOG_SECOND_(a, b, ...) b
#define RP6502_LOG_SECOND(x) RP6502_LOG_SECOND_(x, RP6502_LOG_LEVEL, 0)
#define RP6502_LOG_LEVEL_OF(cat) \
    RP6502_LOG_SECOND(RP6502_LOG_JOIN(RP6502_LOG_PH_, RP6502_LOG_LEVEL_##cat))

/* Guarded the way the pico-sdk guards it, so whichever header arrives first
 * wins and the other skips. */
#ifndef __printflike
#ifdef __GNUC__
#define __printflike(a, b) __attribute__((__format__(__printf__, a, b)))
#else
#define __printflike(a, b)
#endif
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/* The machine's printer: the level and the category in front, the message,
 * and the line ended however this machine ends lines. A message brings no
 * newline of its own. */
__printflike(3, 4) void host_log(int level, const char *category, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#define RP6502_LOG(cat, LEVEL, ...)                                   \
    do                                                                \
    {                                                                 \
        if (RP6502_LOG_##LEVEL <= RP6502_LOG_LEVEL_OF(cat))           \
            host_log(RP6502_LOG_##LEVEL, #cat, __VA_ARGS__);           \
    } while (0)

#endif /* _CORE_SYS_DEBUG_LOG_H_ */
