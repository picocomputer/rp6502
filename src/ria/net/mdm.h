/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MDM_H_
#define _MDM_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    uint8_t echo;
    uint8_t quiet;
    uint8_t verbose;
    uint8_t auto_answer;
    uint8_t esc_char;
    uint8_t cr_char;
    uint8_t lf_char;
    uint8_t bs_char;
    uint8_t s_pointer;
} mdm_settings_t;

/* Kernel events
 */

void mdm_task(void);
void mdm_stop(void);
void mdm_init(void);

/* STDIO
 */

bool mdm_open(const char *);
bool mdm_close(void);
int mdm_rx(char *ch);
int mdm_tx(char ch);

/* utility
 */

extern mdm_settings_t mdm_settings;
int mdm_response_code(char *buf, size_t buf_size, int state);
void mdm_set_response_fn(int (*fn)(char *, size_t, int), int state);

void mdm_factory_settings(mdm_settings_t *settings);
bool mdm_write_settings(const mdm_settings_t *settings);
bool mdm_read_settings(mdm_settings_t *settings);

#endif /* _MDM_H_ */
