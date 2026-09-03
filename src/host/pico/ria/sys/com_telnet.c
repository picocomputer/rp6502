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

#include "core/sys/ria.h"
#include "ria/sys/com.h"
#include "core/sys/config.h"
#include "ria/sys/com_telnet.h"
#include "ria/sys/cfg.h"
#include "ria/sys/vga.h"
#include "ria-w/net/telnet.h"
#include "ria-w/net/cyw.h"
#include "core/str/str.h"
#include "core/str/rln.h"
#include "core/sys/driver.h"
#include "ria-w/net/wifi.h"
#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(DEBUG_SYS) || defined(DEBUG_SYS_COM)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#ifndef RP6502_RIA_W

bool com_telnet_tx_writable(void) { return true; }
void com_telnet_tx_write(char ch) { (void)ch; }
size_t com_telnet_read(char *buf, size_t length)
{
    (void)buf;
    (void)length;
    return 0;
}
int com_telnet_peek(void) { return -1; }
void com_telnet_pump(void) {}
void com_telnet_task(void) {}
bool com_telnet_connected(void) { return false; }
void com_telnet_clear_rx(void) {}

#else


typedef enum
{
    COM_TELNET_STATE_IDLE,
    COM_TELNET_STATE_LISTENING,
    COM_TELNET_STATE_AUTH,
    COM_TELNET_STATE_CONNECTED,
} com_telnet_state_t;
static com_telnet_state_t com_telnet_state = COM_TELNET_STATE_IDLE;
static uint16_t com_telnet_active_port;

static char com_telnet_auth_buf[COM_TELNET_KEY_SIZE];
static uint8_t com_telnet_auth_len;

#define COM_TELNET_TX_BUF_SIZE 32
static char com_telnet_tx_buf[COM_TELNET_TX_BUF_SIZE];
static volatile size_t com_telnet_tx_head;
static volatile size_t com_telnet_tx_tail;

#define COM_TELNET_RX_BUF_SIZE 32
// After this many milliseconds with a full ring and no consume,
// com_telnet_drain_rx drops bytes instead of backpressuring the TCP peer.
#define COM_TELNET_RX_OVERFLOW_MS 5000
static char com_telnet_rx_buf[COM_TELNET_RX_BUF_SIZE];
static size_t com_telnet_rx_head;
static size_t com_telnet_rx_tail;
static absolute_time_t com_telnet_rx_drop_after;

void com_telnet_clear_rx(void)
{
    com_telnet_rx_head = com_telnet_rx_tail = 0;
    com_telnet_rx_drop_after = make_timeout_time_ms(COM_TELNET_RX_OVERFLOW_MS);
}

static void com_telnet_clear_rings(void)
{
    com_telnet_tx_head = com_telnet_tx_tail = 0;
    com_telnet_clear_rx();
}

bool com_telnet_tx_writable(void)
{
    return ((com_telnet_tx_head + 1) % COM_TELNET_TX_BUF_SIZE) != com_telnet_tx_tail;
}

void com_telnet_tx_write(char ch)
{
    com_telnet_tx_head = (com_telnet_tx_head + 1) % COM_TELNET_TX_BUF_SIZE;
    com_telnet_tx_buf[com_telnet_tx_head] = ch;
}

size_t com_telnet_read(char *buf, size_t length)
{
    size_t count = com_recover_rx_char(buf, length, COM_SOURCE_TEL);
    while (count < length && com_telnet_rx_head != com_telnet_rx_tail)
    {
        com_telnet_rx_tail = (com_telnet_rx_tail + 1) % COM_TELNET_RX_BUF_SIZE;
        buf[count++] = com_telnet_rx_buf[com_telnet_rx_tail];
    }
    if (count)
        com_telnet_rx_drop_after = make_timeout_time_ms(COM_TELNET_RX_OVERFLOW_MS);
    return count;
}

int com_telnet_peek(void)
{
    return com_ring_peek((const uint8_t *)com_telnet_rx_buf, COM_TELNET_RX_BUF_SIZE,
                          com_telnet_rx_head, com_telnet_rx_tail);
}

static void com_telnet_drain_tx(void)
{
    if (com_telnet_state != COM_TELNET_STATE_CONNECTED)
    {
        // Discard — nobody to send to
        com_telnet_tx_tail = com_telnet_tx_head;
        return;
    }
    if (com_telnet_tx_tail == com_telnet_tx_head)
        return;
    size_t start = (com_telnet_tx_tail + 1) % COM_TELNET_TX_BUF_SIZE;
    size_t len;
    if (com_telnet_tx_head >= start)
        len = com_telnet_tx_head - start + 1;
    else
        len = COM_TELNET_TX_BUF_SIZE - start;
    uint16_t sent = telnet_tx(NET_TELNET_DESC, &com_telnet_tx_buf[start], len);
    com_telnet_tx_tail = (com_telnet_tx_tail + sent) % COM_TELNET_TX_BUF_SIZE;
}

// Only drives lwIP via cyw_task() when the ring is full and the upstream
// would otherwise stall — cyw_task synchronously fires
// com_telnet_on_accept/on_disconnect callbacks and mutates com_telnet_state +
// the rings, so callers must re-check state after return.
void com_telnet_pump(void)
{
    com_telnet_drain_tx();
    if (!com_telnet_tx_writable())
        cyw_task();
}

static void com_telnet_handle_auth(uint8_t ch)
{
    if (ch == '\b' || ch == 127)
    {
        if (com_telnet_auth_len > 0)
        {
            com_telnet_auth_len--;
            telnet_tx(NET_TELNET_DESC, "\b \b", 3);
        }
    }
    else if (ch == '\r' || ch == '\n')
    {
        com_telnet_auth_buf[com_telnet_auth_len] = 0;
        if (strcmp(com_telnet_auth_buf, com_telnet_get_key()) == 0)
        {
            telnet_tx(NET_TELNET_DESC, STR_TEL_CONNECTED, STR_TEL_CONNECTED_LEN);
            com_telnet_state = COM_TELNET_STATE_CONNECTED;
            vga_set_tel_console_active(true);
            DBG("NET TEL console authenticated\n");
        }
        else
        {
            telnet_tx(NET_TELNET_DESC, STR_TEL_ACCESS_DENIED, STR_TEL_ACCESS_DENIED_LEN);
            DBG("NET TEL console auth failed\n");
            com_telnet_state = COM_TELNET_STATE_LISTENING;
            telnet_close(NET_TELNET_DESC);
        }
    }
    else if (ch >= 32 && ch < 127 && com_telnet_auth_len < COM_TELNET_KEY_SIZE - 1)
    {
        com_telnet_auth_buf[com_telnet_auth_len++] = ch;
        telnet_tx(NET_TELNET_DESC, "*", 1);
    }
}

static void com_telnet_drain_rx(void)
{
    // Default: limit read to ring buffer free space so decoded bytes
    // always fit (decoded <= raw). If the ring has been full and the
    // consumer has been idle for COM_TELNET_RX_OVERFLOW_MS, switch to
    // drop-mode: drain a full scratch buffer from telnet_rx and discard,
    // but still scan discarded bytes for Ctrl-C so a SIGINT during
    // overflow is not lost.
    uint16_t limit = COM_TELNET_RX_BUF_SIZE;
    bool drop_mode = false;
    if (com_telnet_state == COM_TELNET_STATE_CONNECTED)
    {
        size_t used = (com_telnet_rx_head - com_telnet_rx_tail + COM_TELNET_RX_BUF_SIZE) % COM_TELNET_RX_BUF_SIZE;
        size_t free = COM_TELNET_RX_BUF_SIZE - 1 - used;
        if (free == 0)
        {
            if (!time_reached(com_telnet_rx_drop_after))
                return;
            drop_mode = true;
        }
        else
            limit = (uint16_t)free;
    }

    char decoded[COM_TELNET_RX_BUF_SIZE];
    uint16_t decoded_len = telnet_rx(NET_TELNET_DESC, decoded, limit);

    for (uint16_t i = 0; i < decoded_len; i++)
    {
        uint8_t ch = (uint8_t)decoded[i];
        if (com_telnet_state == COM_TELNET_STATE_AUTH)
        {
            com_telnet_handle_auth(ch);
            if (com_telnet_state != COM_TELNET_STATE_AUTH)
                return;
        }
        else if (com_telnet_state == COM_TELNET_STATE_CONNECTED)
        {
            if (ch == 0x03)
                ria_trigger_sigint();
            if (drop_mode)
                continue;
            com_telnet_rx_head = (com_telnet_rx_head + 1) % COM_TELNET_RX_BUF_SIZE;
            com_telnet_rx_buf[com_telnet_rx_head] = ch;
        }
    }

    // NAWS arrives as a side effect of the telnet_rx decode above; relay any
    // fresh size to rln, which reflows the line in place on a resize.
    if (com_telnet_state == COM_TELNET_STATE_CONNECTED)
    {
        uint16_t nw, nh;
        if (telnet_get_naws(NET_TELNET_DESC, &nw, &nh))
            rln_set_naws_size(nw, nh);
    }
}

static bool com_telnet_should_listen(void)
{
    return com_telnet_get_port() > 0 && com_telnet_get_key()[0] != 0 && wifi_ready();
}

// Unified teardown for both full shutdown (target=IDLE, closes the
// listen socket and the session pcb via NET_TELNET_DESC) and peer-driven
// disconnect (target=LISTENING, keeps the listener armed; the session
// pcb close is done by the caller with its own desc). Both targets
// clear the rings and assign the new state.
static void com_telnet_teardown(com_telnet_state_t target)
{
    bool was_session = (com_telnet_state == COM_TELNET_STATE_AUTH || com_telnet_state == COM_TELNET_STATE_CONNECTED);
    bool was_connected = (com_telnet_state == COM_TELNET_STATE_CONNECTED);
    if (was_session && target == COM_TELNET_STATE_IDLE)
        telnet_close(NET_TELNET_DESC);
    if (was_connected && target != COM_TELNET_STATE_CONNECTED)
    {
        vga_set_tel_console_active(false);
        rln_set_naws_size(0, 0); // drop stale telnet geometry
    }
    if (target == COM_TELNET_STATE_IDLE && com_telnet_state != COM_TELNET_STATE_IDLE)
    {
        telnet_listen_close(com_telnet_active_port);
        com_telnet_active_port = 0;
    }
    com_telnet_state = target;
    com_telnet_clear_rings();
}

static void com_telnet_shutdown(void)
{
    com_telnet_teardown(COM_TELNET_STATE_IDLE);
}

static void com_telnet_on_disconnect(int desc)
{
    if (com_telnet_state == COM_TELNET_STATE_AUTH || com_telnet_state == COM_TELNET_STATE_CONNECTED)
    {
        DBG("NET TEL console disconnected\n");
        com_telnet_teardown(COM_TELNET_STATE_LISTENING);
    }
    telnet_close(desc);
}

static bool com_telnet_on_accept(uint16_t port)
{
    if (com_telnet_state != COM_TELNET_STATE_LISTENING)
        return false;

    if (!telnet_accept_server(NET_TELNET_DESC, port, com_telnet_on_disconnect))
        return false;

    telnet_tx(NET_TELNET_DESC, STR_TEL_PASSKEY, STR_TEL_PASSKEY_LEN);

    com_telnet_auth_len = 0;
    com_telnet_clear_rings();
    com_telnet_state = COM_TELNET_STATE_AUTH;
    DBG("NET TEL console accepted, awaiting auth\n");
    return true;
}



/* The task re-derives from the stored value every pass, so there is
 * nothing to apply and every port number is legal. */
int com_telnet_port_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)width;
    if (state == 0)
    {
        bool en = com_telnet_get_port() > 0 && com_telnet_get_key()[0];
        oem_snprintf(buf, buf_size, STR_SET_PORT_RESPONSE, com_telnet_get_port(),
                     en ? S(STR_ENABLED) : S(STR_DISABLED));
        return 1;
    }
    return com_telnet_key_response(buf, buf_size, 0, width);
}

int com_telnet_key_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)state;
    (void)width;
    const char *key = com_telnet_get_key();
    oem_snprintf(buf, buf_size, STR_SET_KEY_RESPONSE,
                 strlen(key) ? S(STR_PARENS_SET) : S(STR_PARENS_NONE));
    return -1;
}



void com_telnet_task(void)
{
    com_telnet_drain_tx();

    if (com_telnet_state != COM_TELNET_STATE_IDLE &&
        (!com_telnet_should_listen() || com_telnet_active_port != com_telnet_get_port()))
    {
        com_telnet_shutdown();
        return;
    }

    switch (com_telnet_state)
    {
    case COM_TELNET_STATE_IDLE:
        if (com_telnet_should_listen() && telnet_listen(com_telnet_get_port(), com_telnet_on_accept))
        {
            com_telnet_active_port = com_telnet_get_port();
            com_telnet_state = COM_TELNET_STATE_LISTENING;
            DBG("NET TEL console listening on port %u\n", com_telnet_get_port());
        }
        break;
    case COM_TELNET_STATE_AUTH:
    case COM_TELNET_STATE_CONNECTED:
        com_telnet_drain_rx();
        break;
    case COM_TELNET_STATE_LISTENING:
        break;
    }
}

bool com_telnet_connected(void)
{
    return com_telnet_state == COM_TELNET_STATE_CONNECTED;
}

#endif
