/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_SYS_LOG_H_
#define _CORE_SYS_LOG_H_

/* Where the machine's diagnostics go. A host that owns its process prints
 * them; a host that is a guest in someone else's — a libretro core in a
 * frontend — hands them to whatever that frontend gives it, and stderr is
 * not its to write. */

/* One finished line, without a trailing newline or a program name: the sink
 * owns both, because only the sink knows what it is writing into. */
typedef void (*log_sink_t)(const char *msg);

/* NULL restores the default sink, which writes "rp6502-emu: <msg>" to stderr. */
void log_set_sink(log_sink_t sink);

/* An error the machine could not act on. There is no other level because
 * there is nothing else to say yet. */
void log_error(const char *fmt, ...);

#endif /* _CORE_SYS_LOG_H_ */
