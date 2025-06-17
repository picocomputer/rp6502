/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _NVR_H_
#define _NVR_H_

#include <stdint.h>
#include <stdbool.h>

typedef struct nvr_settings_t
{
    uint8_t echo;
    uint8_t verbose;
    uint8_t auto_answer;
    uint8_t esc_char;
    uint8_t cr_char;
    uint8_t lf_char;
    uint8_t bs_char;
    uint8_t s_pointer;
} nvr_settings_t;

void nvr_factory_reset(nvr_settings_t *settings);
bool nvr_write(const nvr_settings_t *);
bool nvr_read(nvr_settings_t *);

#endif /* _NVR_H_ */
