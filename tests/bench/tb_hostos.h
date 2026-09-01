/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Two host helpers only tests call. They were in osal/os.h, which is the
 * machine's contract, and the machine has never wanted a temp directory or an
 * environment variable -- the tests do, to stand up a scratch MSC0: and to pin
 * TZ and LC_ALL before asking the clock what time it is.
 *
 * They keep the host_ prefix -- they answer for the host, they are simply not
 * part of the contract every host owes the machine. The bodies are in
 * tb_hostos.c beside this, so a shipped binary carries neither.
 *
 * TEST_PATH_MAX is here for the same reason. A scratch path on the machine
 * running these tests is as long as that OS allows, and a test can say so with
 * a number where shipped code cannot -- which is why HOST_MAX_PATH, the number
 * that used to be said everywhere, is gone.
 */

#ifndef _TESTS_BENCH_TB_HOSTOS_H_
#define _TESTS_BENCH_TB_HOSTOS_H_

#include <stdbool.h>
#include <stddef.h>

#define TEST_PATH_MAX 4096 /* a scratch path on the host running the tests */

bool host_make_tmpdir(char *buf, size_t sz);           /* a fresh empty temp dir, '/'-separated */
void host_setenv(const char *name, const char *value); /* setenv(name, value, 1) in the host spelling */

#endif /* _TESTS_BENCH_TB_HOSTOS_H_ */
