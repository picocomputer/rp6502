/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_BENCH_TB_SEED_H_
#define _TESTS_BENCH_TB_SEED_H_

#include <stdbool.h>

/* Whether the test bench's host is a Mac (host_mac_keys), false until a
 * test sets it. */
extern bool tb_mac_keys;

/* The test bench's host has a clipboard of its own, as a desktop's is, and
 * this is what was put there. */
const char *tb_clipboard_text(void);

#endif /* _TESTS_BENCH_TB_SEED_H_ */
