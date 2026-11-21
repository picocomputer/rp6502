/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One macro: RP6502_LOG(category, LEVEL, fmt, ...). What a build carries is
 * decided when it is configured, by src/core/log.cmake. A call above the level
 * in force is an if over two constants, so an optimizing build folds it away
 * with its format string; a -O0 build keeps both.
 */

#ifndef _CORE_SYS_DEBUG_LOG_H_
#define _CORE_SYS_DEBUG_LOG_H_

#define RP6502_LOG_NONE 0
#define RP6502_LOG_ERROR 1
#define RP6502_LOG_WARN 2
#define RP6502_LOG_INFO 3
#define RP6502_LOG_DEBUG 4
#define RP6502_LOG_LEVEL_NAMES {"", "ERROR", "WARN", "INFO", "DEBUG"}
#ifndef RP6502_LOG_LEVEL
#error "RP6502_LOG_LEVEL"
#endif

/* A category's level is RP6502_LOG_LEVEL_<category> when the build defined
 * one, else RP6502_LOG_LEVEL. A defined name pastes onto a placeholder macro
 * that expands to two arguments, and an undefined one pastes into a name that
 * is no macro at all and stays one argument, so the second argument is either
 * the category's level or the default. */
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

/* Each machine defines this. A message brings no newline of its own, because
 * host_log ends the line the way this machine ends lines. */
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
