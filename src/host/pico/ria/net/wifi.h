/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_WIFI_H_
#define _RIA_NET_WIFI_H_

#define WIFI_SSID_SIZE 33
#define WIFI_PASS_SIZE 65

/* Wi-Fi driver.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void wifi_task(void);

/* Utility
 */

int wifi_scan_response(char *buf, size_t buf_size, int state, unsigned width);
int wifi_status_response(char *buf, size_t buf_size, int state, unsigned width);

void wifi_shutdown(void);
bool wifi_ready(void);
bool wifi_connecting(void);

// Configuration setting SSID
void wifi_load_ssid(const char *str);
bool wifi_set_ssid(const char *ssid);
const char *wifi_get_ssid(void);

// Configuration setting PASS
void wifi_load_pass(const char *str);
bool wifi_set_pass(const char *pass);
const char *wifi_get_pass(void);

#endif /* _RIA_NET_WIFI_H_ */
