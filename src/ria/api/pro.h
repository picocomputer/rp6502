/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_PRO_H_
#define _RIA_API_PRO_H_

/* The process manager handles argv and launching other ROMs.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* argv management
 */

uint16_t pro_argv_count(void);
void pro_argv_clear(void);
bool pro_argv_append(const char *str);
bool pro_argv_replace(uint16_t idx, const char *str);
const char *pro_argv_index(uint16_t idx);

/* The API implementation
 */

bool pro_api_argv(void);
bool pro_api_exec(void);

// Records the current argv[0] as the running
// process, then calls main_start().
void pro_run(void);

// Load a ROM via NFC
void pro_nfc(const uint8_t *ndef, size_t len);


#endif /* _RIA_API_PRO_H_ */
