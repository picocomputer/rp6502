/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W
#include "sys/rem.h"
#include <pico/stdlib.h>
void rem_init(void) {}
void rem_task(void) {}
bool rem_tx_writable(void) { return true; }
void rem_tx_write(char ch) { (void)ch; }
void rem_pump(void) {}
void rem_flush(void) {}
int rem_rx(char *buf, int length)
{
    (void)buf;
    (void)length;
    return PICO_ERROR_NO_DATA;
}
uint16_t rem_get_port(void) { return 0; }
const char *rem_get_key(void) { return ""; }
void rem_load_port(const char *str) { (void)str; }
void rem_load_key(const char *str) { (void)str; }
bool rem_set_port(uint32_t port)
{
    (void)port;
    return false;
}
bool rem_set_key(const char *key)
{
    (void)key;
    return false;
}
#else

#include "sys/rem.h"
#include "net/cyw.h"
#include "net/net.h"
#include "net/wfi.h"
#include "sys/cfg.h"
#include <pico/stdlib.h>
#include <string.h>
#include <stdlib.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_REM)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Telnet protocol constants
#define REM_IAC 255
#define REM_DONT 254
#define REM_DO 253
#define REM_WONT 252
#define REM_WILL 251
#define REM_SB 250
#define REM_SE 240
#define REM_OPT_ECHO 1
#define REM_OPT_SGA 3

// Settings
#define REM_KEY_SIZE 33
static uint16_t rem_port;
static char rem_key[REM_KEY_SIZE];

// State machine
typedef enum
{
    rem_state_idle,
    rem_state_listening,
    rem_state_auth,
    rem_state_connected,
} rem_state_t;
static rem_state_t rem_state;
static uint16_t rem_active_port;

// Auth
static char rem_auth_buf[REM_KEY_SIZE];
static uint8_t rem_auth_len;

// TX ring buffer (console output -> telnet)
#define REM_TX_BUF_SIZE 32
static char rem_tx_buf[REM_TX_BUF_SIZE];
static volatile size_t rem_tx_head;
static volatile size_t rem_tx_tail;

// RX ring buffer (telnet input -> console)
#define REM_RX_BUF_SIZE 32
static char rem_rx_buf[REM_RX_BUF_SIZE];
static size_t rem_rx_head;
static size_t rem_rx_tail;

// Telnet protocol RX state
typedef enum
{
    rem_tel_data,
    rem_tel_iac,
    rem_tel_will,
    rem_tel_wont,
    rem_tel_do,
    rem_tel_dont,
    rem_tel_sb,
    rem_tel_sb_iac,
} rem_tel_state_t;
static rem_tel_state_t rem_tel_state;
static bool rem_last_rx_was_cr;

// -- Settings --

void rem_load_port(const char *str)
{
    rem_port = atoi(str);
}

void rem_load_key(const char *str)
{
    size_t n = strlen(str);
    if (n < REM_KEY_SIZE)
    {
        memcpy(rem_key, str, n);
        rem_key[n] = 0;
    }
}

static void rem_shutdown(void);

bool rem_set_port(uint32_t port)
{
    if (port > 65535)
        return false;
    if (rem_port != (uint16_t)port)
    {
        rem_port = port;
        rem_shutdown();
        cfg_save();
    }
    return true;
}

bool rem_set_key(const char *key)
{
    if (strlen(key) >= REM_KEY_SIZE)
        return false;
    if (strcmp(rem_key, key))
    {
        strncpy(rem_key, key, REM_KEY_SIZE);
        rem_shutdown();
        cfg_save();
    }
    return true;
}

uint16_t rem_get_port(void)
{
    return rem_port;
}

const char *rem_get_key(void)
{
    return rem_key;
}

// -- Telnet protocol helpers --

static void rem_send(const char *buf, uint16_t len)
{
    net_tx(SYS_TEL_DESC, buf, len);
}

static void rem_send_str(const char *str)
{
    rem_send(str, strlen(str));
}

static void rem_negotiate(void)
{
    // Server-side: we echo, suppress go-ahead
    char neg[] = {
        REM_IAC, REM_WILL, REM_OPT_ECHO,
        REM_IAC, REM_WILL, REM_OPT_SGA,
        REM_IAC, REM_DO, REM_OPT_SGA,
    };
    rem_send(neg, sizeof(neg));
}

// Strip telnet protocol, return data byte or -1
static int rem_tel_process(uint8_t byte)
{
    switch (rem_tel_state)
    {
    case rem_tel_data:
        if (byte == REM_IAC)
        {
            rem_tel_state = rem_tel_iac;
            return -1;
        }
        if (rem_last_rx_was_cr && byte == 0)
        {
            rem_last_rx_was_cr = false;
            return -1;
        }
        rem_last_rx_was_cr = (byte == '\r');
        return byte;

    case rem_tel_iac:
        rem_tel_state = rem_tel_data;
        switch (byte)
        {
        case REM_IAC:
            return 0xFF;
        case REM_WILL:
            rem_tel_state = rem_tel_will;
            return -1;
        case REM_WONT:
            rem_tel_state = rem_tel_wont;
            return -1;
        case REM_DO:
            rem_tel_state = rem_tel_do;
            return -1;
        case REM_DONT:
            rem_tel_state = rem_tel_dont;
            return -1;
        case REM_SB:
            rem_tel_state = rem_tel_sb;
            return -1;
        default:
            return -1;
        }

    case rem_tel_will:
    case rem_tel_wont:
    case rem_tel_do:
    case rem_tel_dont:
        rem_tel_state = rem_tel_data;
        return -1;

    case rem_tel_sb:
        if (byte == REM_IAC)
            rem_tel_state = rem_tel_sb_iac;
        return -1;

    case rem_tel_sb_iac:
        rem_tel_state = (byte == REM_SE) ? rem_tel_data : rem_tel_sb;
        return -1;
    }
    return -1;
}

// -- Connection management --

static void rem_on_disconnect(int desc)
{
    (void)desc;
    if (rem_state == rem_state_auth || rem_state == rem_state_connected)
    {
        DBG("SYS REM disconnected\n");
        rem_state = rem_state_listening;
        rem_tx_head = rem_tx_tail = 0;
        rem_rx_head = rem_rx_tail = 0;
    }
}

static bool rem_on_accept(uint16_t port)
{
    if (rem_state != rem_state_listening)
        return false;

    if (!net_accept(SYS_TEL_DESC, port, rem_on_disconnect))
        return false;

    rem_negotiate();
    rem_send_str("\r\nPasskey: ");

    rem_auth_len = 0;
    rem_tel_state = rem_tel_data;
    rem_last_rx_was_cr = false;
    rem_tx_head = rem_tx_tail = 0;
    rem_rx_head = rem_rx_tail = 0;
    rem_state = rem_state_auth;
    DBG("SYS REM connection accepted, awaiting auth\n");
    return true;
}

static void rem_shutdown(void)
{
    if (rem_state == rem_state_auth || rem_state == rem_state_connected)
    {
        rem_state = rem_state_idle;
        net_close(SYS_TEL_DESC);
    }
    if (rem_state == rem_state_listening)
    {
        net_listen_close(rem_active_port);
        rem_active_port = 0;
        rem_state = rem_state_idle;
    }
    rem_tx_head = rem_tx_tail = 0;
    rem_rx_head = rem_rx_tail = 0;
}

static bool rem_should_listen(void)
{
    return rem_port > 0 && rem_key[0] != 0 && wfi_ready();
}

// -- Auth state machine --

static void rem_auth_process(uint8_t ch)
{
    if (ch == '\b' || ch == 127)
    {
        if (rem_auth_len > 0)
        {
            rem_auth_len--;
            rem_send_str("\b \b");
        }
    }
    else if (ch == '\r' || ch == '\n')
    {
        rem_auth_buf[rem_auth_len] = 0;
        if (strcmp(rem_auth_buf, rem_key) == 0)
        {
            rem_send_str("\r\nConnected.\r\n");
            rem_state = rem_state_connected;
            DBG("SYS REM authenticated\n");
        }
        else
        {
            rem_send_str("\r\nAccess denied.\r\n");
            DBG("SYS REM auth failed\n");
            rem_state = rem_state_listening;
            net_close(SYS_TEL_DESC);
        }
    }
    else if (ch >= 32 && rem_auth_len < REM_KEY_SIZE - 1)
    {
        rem_auth_buf[rem_auth_len++] = ch;
        rem_send_str("*");
    }
}

// -- Task: drain network RX --

static void rem_drain_rx(void)
{
    char raw[32];
    uint16_t n = net_rx(SYS_TEL_DESC, raw, sizeof(raw));
    for (uint16_t i = 0; i < n; i++)
    {
        int ch = rem_tel_process((uint8_t)raw[i]);
        if (ch < 0)
            continue;

        if (rem_state == rem_state_auth)
        {
            rem_auth_process(ch);
            if (rem_state != rem_state_auth)
                return;
        }
        else if (rem_state == rem_state_connected)
        {
            if (((rem_rx_head + 1) % REM_RX_BUF_SIZE) != rem_rx_tail)
            {
                rem_rx_head = (rem_rx_head + 1) % REM_RX_BUF_SIZE;
                rem_rx_buf[rem_rx_head] = ch;
            }
        }
    }
}

// -- Task: drain TX buffer to network --

static void rem_drain_tx(void)
{
    if (rem_state != rem_state_connected)
    {
        // Discard — nobody to send to
        rem_tx_tail = rem_tx_head;
        return;
    }
    if (rem_tx_tail == rem_tx_head)
        return;
    size_t start = (rem_tx_tail + 1) % REM_TX_BUF_SIZE;
    size_t len;
    if (rem_tx_head >= start)
        len = rem_tx_head - start + 1;
    else
        len = REM_TX_BUF_SIZE - start;
    uint16_t sent = net_tx(SYS_TEL_DESC, &rem_tx_buf[start], len);
    rem_tx_tail = (rem_tx_tail + sent) % REM_TX_BUF_SIZE;
}

// -- TX for tee --

bool rem_tx_writable(void)
{
    return ((rem_tx_head + 1) % REM_TX_BUF_SIZE) != rem_tx_tail;
}

void rem_tx_write(char ch)
{
    rem_tx_head = (rem_tx_head + 1) % REM_TX_BUF_SIZE;
    rem_tx_buf[rem_tx_head] = ch;
}

// -- Public interface --

void rem_pump(void)
{
    rem_drain_tx();
    if (rem_tx_head != rem_tx_tail)
        cyw_task();
}

void rem_flush(void)
{
    while (rem_state == rem_state_connected && rem_tx_head != rem_tx_tail)
        rem_pump();
}

int rem_rx(char *buf, int length)
{
    int count = 0;
    while (count < length && rem_rx_head != rem_rx_tail)
    {
        rem_rx_tail = (rem_rx_tail + 1) % REM_RX_BUF_SIZE;
        buf[count++] = rem_rx_buf[rem_rx_tail];
    }
    return count ? count : PICO_ERROR_NO_DATA;
}

void rem_init(void)
{
    rem_state = rem_state_idle;
}

void rem_task(void)
{
    rem_drain_tx();
    switch (rem_state)
    {
    case rem_state_idle:
        if (rem_should_listen())
        {
            if (net_listen(rem_port, rem_on_accept))
            {
                rem_active_port = rem_port;
                rem_state = rem_state_listening;
                DBG("SYS REM listening on port %u\n", rem_port);
            }
        }
        break;
    case rem_state_listening:
        if (!rem_should_listen())
            rem_shutdown();
        break;
    case rem_state_auth:
        rem_drain_rx();
        break;
    case rem_state_connected:
        rem_drain_rx();
        break;
    }
}

#endif /* RP6502_RIA_W */
