/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Two host helpers only tests call. They were in core/host.h, which is the
 * machine's contract, and the machine has never wanted a temp directory or an
 * environment variable -- the tests do, to stand up a scratch MSC0: and to pin
 * TZ and LC_ALL before asking the clock what time it is.
 *
 * They keep the host_ prefix and their bodies stay in host/<os>/host.c, where
 * the Win32 spellings need oem_from_wide and win_to_slash. So this header is
 * named for the tests that use it rather than for the file that defines it.
 */

#ifndef _TESTS_BENCH_TB_HOSTOS_H_
#define _TESTS_BENCH_TB_HOSTOS_H_

#include <stdbool.h>
#include <stddef.h>

bool host_make_tmpdir(char *buf, size_t sz);           /* a fresh empty temp dir, '/'-separated */
void host_setenv(const char *name, const char *value); /* setenv(name, value, 1) in the host spelling */

#endif /* _TESTS_BENCH_TB_HOSTOS_H_ */
