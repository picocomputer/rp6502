/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/ria.h"
#include "ria/sys/com.h"
#include "core/sys/config.h"
#include "core/sys/debug_log.h"
#include "ria/sys/com_telnet.h"
#include "ria/sys/cfg.h"
#include "ria/sys/vga.h"
#include "ria-w/net/telnet.h"
#include "ria-w/net/cyw.h"
#include "core/str/str.h"
#include "core/str/rln.h"
#include "core/sys/driver.h"
#include "core/sys/ring.h"
#include "core/sys/timer.h"
#include "ria-w/net/wifi.h"
#include "machine.h"
#include <pico/stdlib.h>
#include <string.h>

#ifndef RP6502_RIA_W

bool com_telnet_tx_writable(void) { return true; }
void com_telnet_tx_write(char ch) { (void)ch; }
void com_telnet_pump(void) {}
void com_telnet_task(void) {}

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
static uint8_t com_telnet_auth_end; // the CR or LF that ended the passkey

static RING(char, 32) com_telnet_tx;
static RING(uint8_t, 32) com_telnet_rx;
static bool com_telnet_rx_held;
static timer_deadline_t com_telnet_rx_hold;

void com_telnet_clear_rx(void)
{
    if (com_telnet_state == COM_TELNET_STATE_CONNECTED)
    {
        char scratch[16];
        while (telnet_rx(NET_TELNET_DESC, scratch, sizeof scratch))
            ;
    }
    RING_SKIP(&com_telnet_rx, RING_LEN(&com_telnet_rx));
    com_telnet_rx_held = false;
}

static void com_telnet_clear_rings(void)
{
    RING_RESET(&com_telnet_tx);
    com_telnet_clear_rx();
}

bool com_telnet_tx_writable(void)
{
    return RING_AVAIL(&com_telnet_tx) != 0;
}

void com_telnet_tx_write(char ch)
{
    RING_PUT(&com_telnet_tx, ch);
}

size_t com_telnet_read(char *buf, size_t length)
{
    return RING_OUT(&com_telnet_rx, buf, (uint32_t)length);
}

int com_telnet_peek(void)
{
    return RING_LEN(&com_telnet_rx) ? *RING_AT(&com_telnet_rx, 0) : -1;
}

static void com_telnet_drain_tx(void)
{
    if (com_telnet_state != COM_TELNET_STATE_CONNECTED)
    {
        // Discard — nobody to send to
        RING_SKIP(&com_telnet_tx, RING_LEN(&com_telnet_tx));
        return;
    }
    const uint32_t len = RING_LINEAR(&com_telnet_tx);
    if (!len)
        return;
    RING_SKIP(&com_telnet_tx, telnet_tx(NET_TELNET_DESC, RING_AT(&com_telnet_tx, 0), len));
}

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
            com_telnet_auth_end = ch;
            vga_set_tel_console_active(true);
            RP6502_LOG(telnet, INFO, "console authenticated");
        }
        else
        {
            telnet_tx(NET_TELNET_DESC, STR_TEL_ACCESS_DENIED, STR_TEL_ACCESS_DENIED_LEN);
            RP6502_LOG(telnet, WARN, "console auth failed");
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
    // A full ring loses nothing for COM_WIRE_HOLD_MS, because net_rx calls
    // tcp_recved only for the bytes it returns, so TCP flow control holds back
    // the peer while the connection is not read. After that, input is read and
    // dropped but still decoded, so a Ctrl-C or a telnet Interrupt Process
    // raises SIGINT.
    uint16_t limit = RING_SIZE(&com_telnet_rx);
    bool drop_mode = false;
    if (com_telnet_state == COM_TELNET_STATE_CONNECTED)
    {
        const uint32_t free = RING_AVAIL(&com_telnet_rx);
        if (free == 0)
        {
            if (!com_telnet_rx_held)
            {
                com_telnet_rx_held = true;
                com_telnet_rx_hold = timer_in_ms(COM_WIRE_HOLD_MS);
            }
            if (!timer_passed(com_telnet_rx_hold))
                return;
            drop_mode = true;
        }
        else
        {
            com_telnet_rx_held = false;
            limit = (uint16_t)free;
        }
    }

    char decoded[RING_SIZE(&com_telnet_rx)];
    uint16_t decoded_len = telnet_rx(NET_TELNET_DESC, decoded, limit);

    for (uint16_t i = 0; i < decoded_len; i++)
    {
        uint8_t ch = (uint8_t)decoded[i];
        if (com_telnet_state == COM_TELNET_STATE_AUTH)
        {
            com_telnet_handle_auth(ch);
            if (com_telnet_state == COM_TELNET_STATE_LISTENING)
                return;
        }
        else if (com_telnet_state == COM_TELNET_STATE_CONNECTED)
        {
            // The other half of the passkey's CR LF is not input.
            uint8_t end = com_telnet_auth_end;
            com_telnet_auth_end = 0;
            if ((ch == '\r' || ch == '\n') && end && ch != end)
                continue;
            if (ch == 0x03)
                ria_trigger_sigint();
            if (drop_mode)
                continue;
            RING_PUT(&com_telnet_rx, ch);
        }
    }

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

static void com_telnet_teardown(com_telnet_state_t target)
{
    bool was_session = (com_telnet_state == COM_TELNET_STATE_AUTH || com_telnet_state == COM_TELNET_STATE_CONNECTED);
    bool was_connected = (com_telnet_state == COM_TELNET_STATE_CONNECTED);
    if (was_session && target == COM_TELNET_STATE_IDLE)
        telnet_close(NET_TELNET_DESC);
    if (was_connected && target != COM_TELNET_STATE_CONNECTED)
    {
        vga_set_tel_console_active(false);
        rln_set_naws_size(0, 0);
        rln_forget_source(COM_SOURCE_TEL);
    }
    if (target == COM_TELNET_STATE_IDLE && com_telnet_state != COM_TELNET_STATE_IDLE)
    {
        telnet_listen_close(com_telnet_active_port);
        com_telnet_active_port = 0;
    }
    com_telnet_state = target;
    com_telnet_clear_rings();
    // The byte the session left in the RX latch goes with it.
    char waiting;
    com_rx_reclaim(&waiting, 1, COM_SOURCE_TEL);
}

static void com_telnet_shutdown(void)
{
    com_telnet_teardown(COM_TELNET_STATE_IDLE);
}

static void com_telnet_on_disconnect(int desc)
{
    if (com_telnet_state == COM_TELNET_STATE_AUTH || com_telnet_state == COM_TELNET_STATE_CONNECTED)
    {
        RP6502_LOG(telnet, INFO, "console disconnected");
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
    com_telnet_auth_end = 0;
    com_telnet_clear_rings();
    com_telnet_state = COM_TELNET_STATE_AUTH;
    RP6502_LOG(telnet, INFO, "console accepted, awaiting auth");
    return true;
}



int HOST_IN_FLASH_FUNC("com_telnet_port_response") com_telnet_port_response(char *buf,
                                                                            size_t buf_size,
                                                                            int state,
                                                                            unsigned width)
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

int HOST_IN_FLASH_FUNC("com_telnet_key_response") com_telnet_key_response(char *buf,
                                                                          size_t buf_size,
                                                                          int state, unsigned width)
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
            RP6502_LOG(telnet, INFO, "console listening on port %u", com_telnet_get_port());
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

#endif
