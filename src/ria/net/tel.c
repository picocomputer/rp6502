/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W

#include "net/tel.h"
#include <pico/stdlib.h>

void tel_init(void) {}
void tel_task(void) {}
bool tel_tx_writable(void) { return true; }
void tel_tx_write(char ch) { (void)ch; }
void tel_pump(void) {}
void tel_flush(void) {}

int tel_read(char *buf, int length)
{
    (void)buf;
    (void)length;
    return PICO_ERROR_NO_DATA;
}

uint16_t tel_get_port(void) { return 0; }
const char *tel_get_key(void) { return ""; }
void tel_load_port(const char *str) { (void)str; }
void tel_load_key(const char *str) { (void)str; }

bool tel_set_port(uint32_t port)
{
    (void)port;
    return false;
}

bool tel_set_key(const char *key)
{
    (void)key;
    return false;
}

#else

#include "net/cyw.h"
#include "net/mdm.h"
#include "net/net.h"
#include "net/tel.h"
#include "net/wfi.h"
#include "sys/cfg.h"
#include <pico/stdlib.h>
#include <stdlib.h>
#include <string.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_TEL)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Telnet protocol constants
#define TEL_IAC 255
#define TEL_DONT 254
#define TEL_DO 253
#define TEL_WONT 252
#define TEL_WILL 251
#define TEL_SB 250
#define TEL_SE 240
#define TEL_NOP 241

// Telnet option codes
#define TEL_OPT_BINARY 0
#define TEL_OPT_ECHO 1
#define TEL_OPT_SGA 3
#define TEL_OPT_TTYPE 24

// Subnegotiation commands
#define TEL_TTYPE_IS 0
#define TEL_TTYPE_SEND 1

// Bitmask positions for tracked options
#define TEL_BIT_BINARY 0x01
#define TEL_BIT_ECHO 0x02
#define TEL_BIT_SGA 0x04
#define TEL_BIT_TTYPE 0x08

typedef enum
{
    tel_rx_data,
    tel_rx_iac,
    tel_rx_will,
    tel_rx_wont,
    tel_rx_do,
    tel_rx_dont,
    tel_rx_sb,
    tel_rx_sb_iac,
} tel_rx_state_t;

typedef struct
{
    tel_rx_state_t rx_state;
    uint8_t sb_buf[48];
    uint8_t sb_len;
    uint8_t opts_we_will;
    uint8_t opts_we_do;
    uint8_t opts_pending_will;
    uint8_t opts_pending_do;
    bool last_rx_was_cr;
    bool telnet_mode;
    bool is_server;
    bool tx_escape_pending;
    uint8_t tx_escape_byte;
    const char *ttype;
} tel_conn_t;

static tel_conn_t tel_conns[NET_MAX_CONNECTIONS];

static uint8_t tel_opt_bit(uint8_t opt)
{
    switch (opt)
    {
    case TEL_OPT_BINARY:
        return TEL_BIT_BINARY;
    case TEL_OPT_ECHO:
        return TEL_BIT_ECHO;
    case TEL_OPT_SGA:
        return TEL_BIT_SGA;
    case TEL_OPT_TTYPE:
        return TEL_BIT_TTYPE;
    default:
        return 0;
    }
}

static bool tel_send_cmd(int desc, uint8_t cmd, uint8_t opt)
{
    char buf[3] = {TEL_IAC, cmd, opt};
    return net_tx(desc, buf, 3) == 3;
}

static const uint8_t tel_tracked_opts[] = {
    TEL_OPT_BINARY, TEL_OPT_ECHO, TEL_OPT_SGA, TEL_OPT_TTYPE};

static void tel_flush_pending(int desc, tel_conn_t *tc)
{
    for (unsigned i = 0; i < sizeof(tel_tracked_opts); i++)
    {
        uint8_t opt = tel_tracked_opts[i];
        uint8_t bit = tel_opt_bit(opt);
        if ((tc->opts_pending_do & bit) &&
            tel_send_cmd(desc, TEL_DO, opt))
        {
            tc->opts_pending_do &= ~bit;
            DBG("NET TEL sent pending DO %d\n", opt);
        }
        if ((tc->opts_pending_will & bit) &&
            tel_send_cmd(desc, TEL_WILL, opt))
        {
            tc->opts_pending_will &= ~bit;
            DBG("NET TEL sent pending WILL %d\n", opt);
        }
    }
}

static void tel_handle_sb(int desc, tel_conn_t *tc)
{
    if (tc->sb_len < 1)
        return;
    if (tc->ttype &&
        tc->sb_buf[0] == TEL_OPT_TTYPE &&
        tc->sb_len >= 2 && tc->sb_buf[1] == TEL_TTYPE_SEND)
    {
        size_t ttype_len = strlen(tc->ttype);
        // IAC SB TTYPE IS <type> IAC SE
        char buf[48];
        size_t pos = 0;
        buf[pos++] = TEL_IAC;
        buf[pos++] = TEL_SB;
        buf[pos++] = TEL_OPT_TTYPE;
        buf[pos++] = TEL_TTYPE_IS;
        if (ttype_len > sizeof(buf) - pos - 2)
            ttype_len = sizeof(buf) - pos - 2;
        memcpy(&buf[pos], tc->ttype, ttype_len);
        pos += ttype_len;
        buf[pos++] = TEL_IAC;
        buf[pos++] = TEL_SE;
        net_tx(desc, buf, pos);
        DBG("NET TEL sent TTYPE IS %s\n", tc->ttype);
    }
}

static void tel_handle_will(int desc, tel_conn_t *tc, uint8_t opt)
{
    uint8_t bit = tel_opt_bit(opt);
    switch (opt)
    {
    case TEL_OPT_ECHO:
    case TEL_OPT_SGA:
    case TEL_OPT_BINARY:
        if (!(tc->opts_we_do & bit))
        {
            tc->opts_we_do |= bit;
            if (tel_send_cmd(desc, TEL_DO, opt))
                DBG("NET TEL sent DO %d\n", opt);
            else
                tc->opts_pending_do |= bit;
        }
        break;
    default:
        tel_send_cmd(desc, TEL_DONT, opt);
        DBG("NET TEL sent DONT %d\n", opt);
        break;
    }
}

static void tel_handle_wont(int desc, tel_conn_t *tc, uint8_t opt)
{
    (void)desc;
    uint8_t bit = tel_opt_bit(opt);
    tc->opts_we_do &= ~bit;
}

static void tel_handle_do(int desc, tel_conn_t *tc, uint8_t opt)
{
    uint8_t bit = tel_opt_bit(opt);
    switch (opt)
    {
    case TEL_OPT_BINARY:
    case TEL_OPT_SGA:
    case TEL_OPT_TTYPE:
    case TEL_OPT_ECHO:
        if (!(tc->opts_we_will & bit))
        {
            tc->opts_we_will |= bit;
            if (tel_send_cmd(desc, TEL_WILL, opt))
                DBG("NET TEL sent WILL %d\n", opt);
            else
                tc->opts_pending_will |= bit;
        }
        break;
    default:
        tel_send_cmd(desc, TEL_WONT, opt);
        DBG("NET TEL sent WONT %d\n", opt);
        break;
    }
}

static void tel_handle_dont(int desc, tel_conn_t *tc, uint8_t opt)
{
    (void)desc;
    uint8_t bit = tel_opt_bit(opt);
    tc->opts_we_will &= ~bit;
}

static void tel_process_rx_byte(int desc, tel_conn_t *tc, uint8_t byte,
                                char *buf, uint16_t *out, uint16_t len)
{
    switch (tc->rx_state)
    {
    case tel_rx_data:
        if (byte == TEL_IAC)
        {
            tc->rx_state = tel_rx_iac;
            return;
        }
        // Strip NUL after CR in telnet mode
        if (tc->telnet_mode && tc->last_rx_was_cr && byte == 0)
        {
            tc->last_rx_was_cr = false;
            return;
        }
        tc->last_rx_was_cr = (byte == '\r');
        if (*out < len)
            buf[(*out)++] = byte;
        return;

    case tel_rx_iac:
        switch (byte)
        {
        case TEL_IAC:
            // Escaped 0xFF - literal data byte
            tc->rx_state = tel_rx_data;
            if (*out < len)
                buf[(*out)++] = (char)0xFF;
            return;
        case TEL_WILL:
            tc->rx_state = tel_rx_will;
            return;
        case TEL_WONT:
            tc->rx_state = tel_rx_wont;
            return;
        case TEL_DO:
            tc->rx_state = tel_rx_do;
            return;
        case TEL_DONT:
            tc->rx_state = tel_rx_dont;
            return;
        case TEL_SB:
            tc->rx_state = tel_rx_sb;
            tc->sb_len = 0;
            return;
        default:
            // NOP or other command - ignore
            tc->rx_state = tel_rx_data;
            return;
        }

    case tel_rx_will:
        tel_handle_will(desc, tc, byte);
        tc->rx_state = tel_rx_data;
        return;

    case tel_rx_wont:
        tel_handle_wont(desc, tc, byte);
        tc->rx_state = tel_rx_data;
        return;

    case tel_rx_do:
        tel_handle_do(desc, tc, byte);
        tc->rx_state = tel_rx_data;
        return;

    case tel_rx_dont:
        tel_handle_dont(desc, tc, byte);
        tc->rx_state = tel_rx_data;
        return;

    case tel_rx_sb:
        if (byte == TEL_IAC)
        {
            tc->rx_state = tel_rx_sb_iac;
            return;
        }
        if (tc->sb_len < sizeof(tc->sb_buf))
            tc->sb_buf[tc->sb_len++] = byte;
        return;

    case tel_rx_sb_iac:
        if (byte == TEL_SE)
        {
            tel_handle_sb(desc, tc);
            tc->rx_state = tel_rx_data;
            return;
        }
        if (byte == TEL_IAC)
        {
            // Escaped 0xFF within subnegotiation
            if (tc->sb_len < sizeof(tc->sb_buf))
                tc->sb_buf[tc->sb_len++] = 0xFF;
            tc->rx_state = tel_rx_sb;
            return;
        }
        // Malformed - treat as end of subneg
        tc->rx_state = tel_rx_data;
        return;
    }
}

// -- Telnet protocol API --

uint16_t tel_rx(int desc, char *buf, uint16_t len)
{
    tel_conn_t *tc = &tel_conns[desc];
    if (!tc->telnet_mode)
        return net_rx(desc, buf, len);

    if (tc->opts_pending_do || tc->opts_pending_will)
        tel_flush_pending(desc, tc);
    uint16_t out = 0;
    while (out < len)
    {
        char raw[64];
        uint16_t want = len - out;
        if (want > sizeof(raw))
            want = sizeof(raw);
        uint16_t n = net_rx(desc, raw, want);
        if (n == 0)
            break;
        for (uint16_t i = 0; i < n; i++)
            tel_process_rx_byte(desc, tc, (uint8_t)raw[i], buf, &out, len);
    }
    return out;
}

uint16_t tel_tx(int desc, const char *buf, uint16_t len)
{
    tel_conn_t *tc = &tel_conns[desc];
    if (!tc->telnet_mode)
        return net_tx(desc, buf, len);

    // Flush the second byte of a split escape sequence
    if (tc->tx_escape_pending)
    {
        char b = tc->tx_escape_byte;
        if (net_tx(desc, &b, 1) != 1)
            return 0;
        tc->tx_escape_pending = false;
    }

    char out[128];
    uint16_t consumed = 0;
    uint16_t out_pos = 0;

    while (consumed < len)
    {
        uint8_t byte = buf[consumed];
        uint16_t needed = 1;
        if (byte == (uint8_t)TEL_IAC)
            needed = 2;
        else if (byte == '\r')
            needed = 2;

        if (out_pos + needed > sizeof(out))
            break;

        out[out_pos++] = byte;
        if (byte == (uint8_t)TEL_IAC)
            out[out_pos++] = (char)TEL_IAC;
        else if (byte == '\r')
            out[out_pos++] = 0;
        consumed++;
    }

    if (out_pos > 0)
    {
        uint16_t sent = net_tx(desc, out, out_pos);
        if (sent < out_pos)
        {
            // Walk back to find how many source bytes were fully sent
            uint16_t adj = 0;
            uint16_t out_walk = 0;
            while (adj < consumed)
            {
                uint8_t b = buf[adj];
                uint16_t step = 1;
                if (b == (uint8_t)TEL_IAC || b == '\r')
                    step = 2;
                if (out_walk + step > sent)
                    break;
                out_walk += step;
                adj++;
            }
            // Stash second byte if an escape pair was split
            if (out_walk < sent)
            {
                tc->tx_escape_pending = true;
                tc->tx_escape_byte = out[sent];
                adj++;
            }
            consumed = adj;
        }
    }
    return consumed;
}

bool tel_open(int desc, const char *hostname, uint16_t port,
              void (*on_close)(int))
{
    return net_open(desc, hostname, port, on_close);
}

static void tel_reset(int desc)
{
    memset(&tel_conns[desc], 0, sizeof(tel_conn_t));
}

void tel_close(int desc)
{
    net_close(desc);
    tel_reset(desc);
}

void tel_negotiate(int desc)
{
    tel_conn_t *tc = &tel_conns[desc];
    tc->telnet_mode = (mdm_settings->net_mode != 0);
    if (!tc->telnet_mode)
        return;

    tc->is_server = false;
    tc->ttype = mdm_settings->tty_type;
    tc->opts_we_will = TEL_BIT_BINARY | TEL_BIT_SGA | TEL_BIT_TTYPE;
    tc->opts_we_do = TEL_BIT_BINARY | TEL_BIT_SGA;
    tc->opts_pending_will = tc->opts_we_will;
    tc->opts_pending_do = tc->opts_we_do;
    tel_flush_pending(desc, tc);
    DBG("NET TEL sent initial negotiation\n");
}

static void tel_negotiate_server(int desc)
{
    tel_conn_t *tc = &tel_conns[desc];
    tc->telnet_mode = true;
    tc->is_server = true;
    tc->ttype = NULL;
    tc->opts_we_will = TEL_BIT_ECHO | TEL_BIT_SGA;
    tc->opts_we_do = TEL_BIT_SGA;
    tc->opts_pending_will = tc->opts_we_will;
    tc->opts_pending_do = tc->opts_we_do;
    tel_flush_pending(desc, tc);
    DBG("NET TEL server negotiation sent\n");
}

bool tel_listen(uint16_t port, net_accept_fn on_accept)
{
    return net_listen(port, on_accept);
}

void tel_listen_close(uint16_t port)
{
    net_listen_close(port);
}

bool tel_accept(int desc, uint16_t port, void (*on_close)(int))
{
    tel_reset(desc);
    if (!net_accept(desc, port, on_close))
        return false;
    tel_negotiate(desc);
    return true;
}

void tel_reject(uint16_t port)
{
    net_reject(port);
}

bool tel_has_pending(uint16_t port)
{
    return net_has_pending(port);
}

// =====================================================================
// Console server
// =====================================================================

// Settings
#define TEL_KEY_SIZE 33
static uint16_t tel_port;
static char tel_key[TEL_KEY_SIZE];

// State machine
typedef enum
{
    tel_state_idle,
    tel_state_listening,
    tel_state_auth,
    tel_state_connected,
} tel_state_t;
static tel_state_t tel_state;
static uint16_t tel_active_port;

// Auth
static char tel_auth_buf[TEL_KEY_SIZE];
static uint8_t tel_auth_len;

// TX ring buffer (console output -> telnet)
#define TEL_TX_BUF_SIZE 32
static char tel_tx_buf[TEL_TX_BUF_SIZE];
static volatile size_t tel_tx_head;
static volatile size_t tel_tx_tail;

// RX ring buffer (telnet input -> console)
#define TEL_RX_BUF_SIZE 32
static char tel_rx_buf[TEL_RX_BUF_SIZE];
static size_t tel_rx_head;
static size_t tel_rx_tail;

// -- Settings --

void tel_load_port(const char *str)
{
    tel_port = atoi(str);
}

void tel_load_key(const char *str)
{
    size_t n = strlen(str);
    if (n < TEL_KEY_SIZE)
    {
        memcpy(tel_key, str, n);
        tel_key[n] = 0;
    }
}

static void tel_shutdown(void);

bool tel_set_port(uint32_t port)
{
    if (port > 65535)
        return false;
    if (tel_port != (uint16_t)port)
    {
        tel_port = port;
        if (port == 0)
            tel_shutdown();
        cfg_save();
    }
    return true;
}

bool tel_set_key(const char *key)
{
    if (strlen(key) >= TEL_KEY_SIZE)
        return false;
    if (strcmp(tel_key, key))
    {
        strncpy(tel_key, key, TEL_KEY_SIZE);
        if (tel_key[0] == 0)
            tel_shutdown();
        cfg_save();
    }
    return true;
}

uint16_t tel_get_port(void)
{
    return tel_port;
}

const char *tel_get_key(void)
{
    return tel_key;
}

// -- Connection management --

static void tel_on_disconnect(int desc)
{
    (void)desc;
    if (tel_state == tel_state_auth || tel_state == tel_state_connected)
    {
        DBG("NET TEL console disconnected\n");
        tel_state = tel_state_listening;
        tel_tx_head = tel_tx_tail = 0;
        tel_rx_head = tel_rx_tail = 0;
    }
}

static bool tel_on_accept(uint16_t port)
{
    if (tel_state != tel_state_listening)
        return false;

    tel_reset(SYS_TEL_DESC);
    if (!net_accept(SYS_TEL_DESC, port, tel_on_disconnect))
        return false;

    tel_negotiate_server(SYS_TEL_DESC);
    tel_tx(SYS_TEL_DESC, "\r\nPasskey: ", 11);

    tel_auth_len = 0;
    tel_tx_head = tel_tx_tail = 0;
    tel_rx_head = tel_rx_tail = 0;
    tel_state = tel_state_auth;
    DBG("NET TEL console accepted, awaiting auth\n");
    return true;
}

static void tel_shutdown(void)
{
    if (tel_state == tel_state_auth || tel_state == tel_state_connected)
        tel_close(SYS_TEL_DESC);
    if (tel_state >= tel_state_listening)
    {
        net_listen_close(tel_active_port);
        tel_active_port = 0;
    }
    tel_state = tel_state_idle;
    tel_tx_head = tel_tx_tail = 0;
    tel_rx_head = tel_rx_tail = 0;
}

static bool tel_should_listen(void)
{
    return tel_port > 0 && tel_key[0] != 0 && wfi_ready();
}

// -- Auth --

static void tel_handle_auth(uint8_t ch)
{
    if (ch == '\b' || ch == 127)
    {
        if (tel_auth_len > 0)
        {
            tel_auth_len--;
            tel_tx(SYS_TEL_DESC, "\b \b", 3);
        }
    }
    else if (ch == '\r' || ch == '\n')
    {
        tel_auth_buf[tel_auth_len] = 0;
        if (strcmp(tel_auth_buf, tel_key) == 0)
        {
            tel_tx(SYS_TEL_DESC, "\r\nConnected.\r\n", 14);
            tel_state = tel_state_connected;
            DBG("NET TEL console authenticated\n");
        }
        else
        {
            tel_tx(SYS_TEL_DESC, "\r\nAccess denied.\r\n", 18);
            DBG("NET TEL console auth failed\n");
            tel_state = tel_state_listening;
            tel_close(SYS_TEL_DESC);
        }
    }
    else if (ch >= 32 && tel_auth_len < TEL_KEY_SIZE - 1)
    {
        tel_auth_buf[tel_auth_len++] = ch;
        tel_tx(SYS_TEL_DESC, "*", 1);
    }
}

// -- Task: drain network RX --

static void tel_drain_rx(void)
{
    char raw[32];
    uint16_t n = net_rx(SYS_TEL_DESC, raw, sizeof(raw));
    if (n == 0)
        return;

    tel_conn_t *tc = &tel_conns[SYS_TEL_DESC];
    char decoded[32];
    uint16_t decoded_len = 0;

    for (uint16_t i = 0; i < n; i++)
        tel_process_rx_byte(SYS_TEL_DESC, tc, (uint8_t)raw[i],
                            decoded, &decoded_len, sizeof(decoded));

    for (uint16_t i = 0; i < decoded_len; i++)
    {
        uint8_t ch = (uint8_t)decoded[i];
        if (tel_state == tel_state_auth)
        {
            tel_handle_auth(ch);
            if (tel_state != tel_state_auth)
                return;
        }
        else if (tel_state == tel_state_connected)
        {
            if (((tel_rx_head + 1) % TEL_RX_BUF_SIZE) != tel_rx_tail)
            {
                tel_rx_head = (tel_rx_head + 1) % TEL_RX_BUF_SIZE;
                tel_rx_buf[tel_rx_head] = ch;
            }
        }
    }
}

// -- Task: drain TX buffer to network --

static void tel_drain_tx(void)
{
    if (tel_state != tel_state_connected)
    {
        // Discard — nobody to send to
        tel_tx_tail = tel_tx_head;
        return;
    }
    if (tel_tx_tail == tel_tx_head)
        return;
    size_t start = (tel_tx_tail + 1) % TEL_TX_BUF_SIZE;
    size_t len;
    if (tel_tx_head >= start)
        len = tel_tx_head - start + 1;
    else
        len = TEL_TX_BUF_SIZE - start;
    uint16_t sent = net_tx(SYS_TEL_DESC, &tel_tx_buf[start], len);
    tel_tx_tail = (tel_tx_tail + sent) % TEL_TX_BUF_SIZE;
}

// -- TX for tee --

bool tel_tx_writable(void)
{
    return ((tel_tx_head + 1) % TEL_TX_BUF_SIZE) != tel_tx_tail;
}

void tel_tx_write(char ch)
{
    tel_tx_head = (tel_tx_head + 1) % TEL_TX_BUF_SIZE;
    tel_tx_buf[tel_tx_head] = ch;
}

// -- Public interface --

void tel_pump(void)
{
    tel_drain_tx();
    if (tel_tx_head != tel_tx_tail)
        cyw_task();
}

void tel_flush(void)
{
    while (tel_state == tel_state_connected && tel_tx_head != tel_tx_tail)
        tel_pump();
}

int tel_read(char *buf, int length)
{
    int count = 0;
    while (count < length && tel_rx_head != tel_rx_tail)
    {
        tel_rx_tail = (tel_rx_tail + 1) % TEL_RX_BUF_SIZE;
        buf[count++] = tel_rx_buf[tel_rx_tail];
    }
    return count ? count : PICO_ERROR_NO_DATA;
}

void tel_init(void)
{
    tel_state = tel_state_idle;
}

void tel_task(void)
{
    tel_drain_tx();
    switch (tel_state)
    {
    case tel_state_idle:
        if (tel_should_listen())
        {
            if (net_listen(tel_port, tel_on_accept))
            {
                tel_active_port = tel_port;
                tel_state = tel_state_listening;
                DBG("NET TEL console listening on port %u\n", tel_port);
            }
        }
        break;
    case tel_state_listening:
        if (!tel_should_listen() || tel_active_port != tel_port)
            tel_shutdown();
        break;
    case tel_state_auth:
        tel_drain_rx();
        break;
    case tel_state_connected:
        tel_drain_rx();
        break;
    }
}

#endif /* RP6502_RIA_W */
