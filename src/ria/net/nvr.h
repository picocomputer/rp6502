/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _NVR_H_
#define _NVR_H_

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    uint8_t verbose;
    uint8_t auto_answer;
} nvr_settings_t;

bool nvr_write(const nvr_settings_t *);
bool nvr_read(nvr_settings_t *);

#endif /* _NVR_H_ */
