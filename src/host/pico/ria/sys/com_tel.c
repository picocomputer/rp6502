/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The console over TCP: a listener, one session at a time, an optional key
 * to get past, and rings that drop rather than backpressure a peer that has
 * stopped reading. A board with no radio answers the same calls with
 * nothing.
 *
 * This is a source the console picks between, not the console: com.c owns
 * the picking, this owns the socket.
 */

#include "ria/sys/com.h"
#include "ria/sys/com_tel.h"
#include "ria/sys/cfg.h"
#include "ria/sys/vga.h"
#include "ria/net/tel.h"
#include "ria/net/cyw.h"
#include "core/str/str.h"
#include "core/str/rln.h"
#include "core/main.h"
#include "ria/net/wfi.h"
#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_COM)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#ifndef RP6502_RIA_W

bool com_tel_tx_writable(void) { return true; }
void com_tel_tx_write(char ch) { (void)ch; }
size_t com_tel_read(char *buf, size_t length)
{
    (void)buf;
    (void)length;
    return 0;
}
int com_tel_peek(void) { return -1; }
void com_tel_pump(void) {}
void com_tel_task(void) {}
bool com_tel_connected(void) { return false; }
void com_tel_clear_rx(void) {}

#else

#define COM_TEL_KEY_SIZE 33
static uint16_t com_tel_port = 23;
static char com_tel_key[COM_TEL_KEY_SIZE];

typedef enum
{
    COM_TEL_STATE_IDLE,
    COM_TEL_STATE_LISTENING,
    COM_TEL_STATE_AUTH,
    COM_TEL_STATE_CONNECTED,
} com_tel_state_t;
static com_tel_state_t com_tel_state = COM_TEL_STATE_IDLE;
static uint16_t com_tel_active_port;

static char com_tel_auth_buf[COM_TEL_KEY_SIZE];
static uint8_t com_tel_auth_len;

#define COM_TEL_TX_BUF_SIZE 32
static char com_tel_tx_buf[COM_TEL_TX_BUF_SIZE];
static volatile size_t com_tel_tx_head;
static volatile size_t com_tel_tx_tail;

#define COM_TEL_RX_BUF_SIZE 32
// After this many milliseconds with a full ring and no consume,
// com_tel_drain_rx drops bytes instead of backpressuring the TCP peer.
#define COM_TEL_RX_OVERFLOW_MS 5000
static char com_tel_rx_buf[COM_TEL_RX_BUF_SIZE];
static size_t com_tel_rx_head;
static size_t com_tel_rx_tail;
static absolute_time_t com_tel_rx_drop_after;

void com_tel_clear_rx(void)
{
    com_tel_rx_head = com_tel_rx_tail = 0;
    com_tel_rx_drop_after = make_timeout_time_ms(COM_TEL_RX_OVERFLOW_MS);
}

static void com_tel_clear_rings(void)
{
    com_tel_tx_head = com_tel_tx_tail = 0;
    com_tel_clear_rx();
}

bool com_tel_tx_writable(void)
{
    return ((com_tel_tx_head + 1) % COM_TEL_TX_BUF_SIZE) != com_tel_tx_tail;
}

void com_tel_tx_write(char ch)
{
    com_tel_tx_head = (com_tel_tx_head + 1) % COM_TEL_TX_BUF_SIZE;
    com_tel_tx_buf[com_tel_tx_head] = ch;
}

size_t com_tel_read(char *buf, size_t length)
{
    size_t count = com_recover_rx_char(buf, COM_SOURCE_TEL);
    while (count < length && com_tel_rx_head != com_tel_rx_tail)
    {
        com_tel_rx_tail = (com_tel_rx_tail + 1) % COM_TEL_RX_BUF_SIZE;
        buf[count++] = com_tel_rx_buf[com_tel_rx_tail];
    }
    if (count)
        com_tel_rx_drop_after = make_timeout_time_ms(COM_TEL_RX_OVERFLOW_MS);
    return count;
}

int com_tel_peek(void)
{
    return com_ring_peek((const uint8_t *)com_tel_rx_buf, COM_TEL_RX_BUF_SIZE,
                         com_tel_rx_head, com_tel_rx_tail);
}

static void com_tel_drain_tx(void)
{
    if (com_tel_state != COM_TEL_STATE_CONNECTED)
    {
        // Discard — nobody to send to
        com_tel_tx_tail = com_tel_tx_head;
        return;
    }
    if (com_tel_tx_tail == com_tel_tx_head)
        return;
    size_t start = (com_tel_tx_tail + 1) % COM_TEL_TX_BUF_SIZE;
    size_t len;
    if (com_tel_tx_head >= start)
        len = com_tel_tx_head - start + 1;
    else
        len = COM_TEL_TX_BUF_SIZE - start;
    uint16_t sent = tel_tx(SYS_TEL_DESC, &com_tel_tx_buf[start], len);
    com_tel_tx_tail = (com_tel_tx_tail + sent) % COM_TEL_TX_BUF_SIZE;
}

// Only drives lwIP via cyw_task() when the ring is full and the upstream
// would otherwise stall — cyw_task synchronously fires
// com_tel_on_accept/on_disconnect callbacks and mutates com_tel_state +
// the rings, so callers must re-check state after return.
void com_tel_pump(void)
{
    com_tel_drain_tx();
    if (!com_tel_tx_writable())
        cyw_task();
}

static void com_tel_handle_auth(uint8_t ch)
{
    if (ch == '\b' || ch == 127)
    {
        if (com_tel_auth_len > 0)
        {
            com_tel_auth_len--;
            tel_tx(SYS_TEL_DESC, "\b \b", 3);
        }
    }
    else if (ch == '\r' || ch == '\n')
    {
        com_tel_auth_buf[com_tel_auth_len] = 0;
        if (strcmp(com_tel_auth_buf, com_tel_key) == 0)
        {
            tel_tx(SYS_TEL_DESC, STR_TEL_CONNECTED, STR_TEL_CONNECTED_LEN);
            com_tel_state = COM_TEL_STATE_CONNECTED;
            vga_set_tel_console_active(true);
            DBG("NET TEL console authenticated\n");
        }
        else
        {
            tel_tx(SYS_TEL_DESC, STR_TEL_ACCESS_DENIED, STR_TEL_ACCESS_DENIED_LEN);
            DBG("NET TEL console auth failed\n");
            com_tel_state = COM_TEL_STATE_LISTENING;
            tel_close(SYS_TEL_DESC);
        }
    }
    else if (ch >= 32 && ch < 127 && com_tel_auth_len < COM_TEL_KEY_SIZE - 1)
    {
        com_tel_auth_buf[com_tel_auth_len++] = ch;
        tel_tx(SYS_TEL_DESC, "*", 1);
    }
}

static void com_tel_drain_rx(void)
{
    // Default: limit read to ring buffer free space so decoded bytes
    // always fit (decoded <= raw). If the ring has been full and the
    // consumer has been idle for COM_TEL_RX_OVERFLOW_MS, switch to
    // drop-mode: drain a full scratch buffer from tel_rx and discard,
    // but still scan discarded bytes for Ctrl-C so a SIGINT during
    // overflow is not lost.
    uint16_t limit = COM_TEL_RX_BUF_SIZE;
    bool drop_mode = false;
    if (com_tel_state == COM_TEL_STATE_CONNECTED)
    {
        size_t used = (com_tel_rx_head - com_tel_rx_tail + COM_TEL_RX_BUF_SIZE) % COM_TEL_RX_BUF_SIZE;
        size_t free = COM_TEL_RX_BUF_SIZE - 1 - used;
        if (free == 0)
        {
            if (!time_reached(com_tel_rx_drop_after))
                return;
            drop_mode = true;
        }
        else
            limit = (uint16_t)free;
    }

    char decoded[COM_TEL_RX_BUF_SIZE];
    uint16_t decoded_len = tel_rx(SYS_TEL_DESC, decoded, limit);

    for (uint16_t i = 0; i < decoded_len; i++)
    {
        uint8_t ch = (uint8_t)decoded[i];
        if (com_tel_state == COM_TEL_STATE_AUTH)
        {
            com_tel_handle_auth(ch);
            if (com_tel_state != COM_TEL_STATE_AUTH)
                return;
        }
        else if (com_tel_state == COM_TEL_STATE_CONNECTED)
        {
            if (ch == 0x03)
                ria_trigger_sigint();
            if (drop_mode)
                continue;
            com_tel_rx_head = (com_tel_rx_head + 1) % COM_TEL_RX_BUF_SIZE;
            com_tel_rx_buf[com_tel_rx_head] = ch;
        }
    }

    // NAWS arrives as a side effect of the tel_rx decode above; relay any
    // fresh size to rln, which reflows the line in place on a resize.
    if (com_tel_state == COM_TEL_STATE_CONNECTED)
    {
        uint16_t nw, nh;
        if (tel_get_naws(SYS_TEL_DESC, &nw, &nh))
            rln_set_naws_size(nw, nh);
    }
}

static bool com_tel_should_listen(void)
{
    return com_tel_port > 0 && com_tel_key[0] != 0 && wfi_ready();
}

// Unified teardown for both full shutdown (target=IDLE, closes the
// listen socket and the session pcb via SYS_TEL_DESC) and peer-driven
// disconnect (target=LISTENING, keeps the listener armed; the session
// pcb close is done by the caller with its own desc). Both targets
// clear the rings and assign the new state.
static void com_tel_teardown(com_tel_state_t target)
{
    bool was_session = (com_tel_state == COM_TEL_STATE_AUTH || com_tel_state == COM_TEL_STATE_CONNECTED);
    bool was_connected = (com_tel_state == COM_TEL_STATE_CONNECTED);
    if (was_session && target == COM_TEL_STATE_IDLE)
        tel_close(SYS_TEL_DESC);
    if (was_connected && target != COM_TEL_STATE_CONNECTED)
    {
        vga_set_tel_console_active(false);
        rln_set_naws_size(0, 0); // drop stale telnet geometry
    }
    if (target == COM_TEL_STATE_IDLE && com_tel_state != COM_TEL_STATE_IDLE)
    {
        tel_listen_close(com_tel_active_port);
        com_tel_active_port = 0;
    }
    com_tel_state = target;
    com_tel_clear_rings();
}

static void com_tel_shutdown(void)
{
    com_tel_teardown(COM_TEL_STATE_IDLE);
}

static void com_tel_on_disconnect(int desc)
{
    if (com_tel_state == COM_TEL_STATE_AUTH || com_tel_state == COM_TEL_STATE_CONNECTED)
    {
        DBG("NET TEL console disconnected\n");
        com_tel_teardown(COM_TEL_STATE_LISTENING);
    }
    tel_close(desc);
}

static bool com_tel_on_accept(uint16_t port)
{
    if (com_tel_state != COM_TEL_STATE_LISTENING)
        return false;

    if (!tel_accept_server(SYS_TEL_DESC, port, com_tel_on_disconnect))
        return false;

    tel_tx(SYS_TEL_DESC, STR_TEL_PASSKEY, STR_TEL_PASSKEY_LEN);

    com_tel_auth_len = 0;
    com_tel_clear_rings();
    com_tel_state = COM_TEL_STATE_AUTH;
    DBG("NET TEL console accepted, awaiting auth\n");
    return true;
}

void com_tel_load_port(const char *str)
{
    str_parse_uint16(&str, &com_tel_port);
}

void com_tel_load_key(const char *str)
{
    size_t n = strlen(str);
    if (n < COM_TEL_KEY_SIZE)
    {
        memcpy(com_tel_key, str, n);
        com_tel_key[n] = 0;
    }
}

bool com_tel_set_port(uint16_t port)
{
    if (com_tel_port != port)
    {
        com_tel_port = port;
        if (port == 0)
            com_tel_shutdown();
        cfg_save();
    }
    return true;
}

bool com_tel_set_key(const char *key)
{
    size_t n = strlen(key);
    if (n >= COM_TEL_KEY_SIZE)
        return false;
    if (strcmp(com_tel_key, key))
    {
        memcpy(com_tel_key, key, n);
        com_tel_key[n] = 0;
        if (com_tel_key[0] == 0)
            com_tel_shutdown();
        cfg_save();
    }
    return true;
}

uint16_t com_tel_get_port(void)
{
    return com_tel_port;
}

const char *com_tel_get_key(void)
{
    return com_tel_key;
}

void com_tel_task(void)
{
    com_tel_drain_tx();

    if (com_tel_state != COM_TEL_STATE_IDLE &&
        (!com_tel_should_listen() || com_tel_active_port != com_tel_port))
    {
        com_tel_shutdown();
        return;
    }

    switch (com_tel_state)
    {
    case COM_TEL_STATE_IDLE:
        if (com_tel_should_listen() && tel_listen(com_tel_port, com_tel_on_accept))
        {
            com_tel_active_port = com_tel_port;
            com_tel_state = COM_TEL_STATE_LISTENING;
            DBG("NET TEL console listening on port %u\n", com_tel_port);
        }
        break;
    case COM_TEL_STATE_AUTH:
    case COM_TEL_STATE_CONNECTED:
        com_tel_drain_rx();
        break;
    case COM_TEL_STATE_LISTENING:
        break;
    }
}

bool com_tel_connected(void)
{
    return com_tel_state == COM_TEL_STATE_CONNECTED;
}

#endif
