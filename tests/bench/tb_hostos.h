/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_BENCH_TB_HOSTOS_H_
#define _TESTS_BENCH_TB_HOSTOS_H_

#include <stdbool.h>
#include <stddef.h>

#define TEST_PATH_MAX 4096

bool host_make_tmpdir(char *buf, size_t sz);
void host_setenv(const char *name, const char *value);

const char *host_drive(void);

#endif /* _TESTS_BENCH_TB_HOSTOS_H_ */
