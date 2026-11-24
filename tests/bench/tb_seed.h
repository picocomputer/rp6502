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

/* Whether the test bench's host has no clipboard (host_clipboard_get), as
 * the RIA has none, false until a test sets it. */
extern bool tb_no_clipboard;

#endif /* _TESTS_BENCH_TB_SEED_H_ */
