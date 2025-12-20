/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_WFI_H_
#define _RIA_NET_WFI_H_

#define WFI_SSID_SIZE 33
#define WFI_PASS_SIZE 65

/* Wi-Fi driver.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void wfi_task(void);

/* Utility
 */

int wfi_status_response(char *buf, size_t buf_size, int state);
void wfi_shutdown(void);
bool wfi_ready(void);

// Configuration setting SSID
void wfi_load_ssid(const char *str, size_t len);
bool wfi_set_ssid(const char *ssid);
const char *wfi_get_ssid(void);

// Configuration setting PASS
void wfi_load_pass(const char *str, size_t len);
bool wfi_set_pass(const char *pass);
const char *wfi_get_pass(void);

#endif /* _RIA_NET_WFI_H_ */
