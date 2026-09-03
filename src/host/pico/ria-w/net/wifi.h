/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_W_NET_WIFI_H_
#define _RIA_W_NET_WIFI_H_

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
void wifi_apply_ssid(const char *ssid, bool changed);
int wifi_ssid_response(char *buf, size_t buf_size, int state, unsigned width);

// Configuration setting PASS
bool wifi_check_pass(const char *in, char *out);
void wifi_apply_pass(const char *pass, bool changed);
int wifi_pass_response(char *buf, size_t buf_size, int state, unsigned width);

/* This driver's row in a machine's driver list; see core/sys/driver.h. Joins and holds the network; retries on its own timer, so it needs no
 * bring-up beyond the radio cyw already brought up. */
#define WIFI_CONFIG_SSID CONFIG_STR(W, wifi, ssid, WIFI_SSID_SIZE, "", \
    nul_check, wifi_apply_ssid, STR_SSID, wifi_ssid_response, \
    STR_HELP_SET_SSID, wifi_scan_response)
#define WIFI_CONFIG_PASS CONFIG_STR(K, wifi, pass, WIFI_PASS_SIZE, "", \
    wifi_check_pass, wifi_apply_pass, STR_PASS, wifi_pass_response, \
    STR_HELP_SET_PASS, NULL)
#define WIFI_DRIVER DRIVER(nul_init, wifi_task, nul_task, nul_run, nul_stop, nul_break, \
    WIFI_CONFIG_SSID, WIFI_CONFIG_PASS)

#endif /* _RIA_W_NET_WIFI_H_ */
