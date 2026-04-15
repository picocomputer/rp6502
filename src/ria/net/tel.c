/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RP6502_RIA_W

#include "net/mdm.h"
#include "net/net.h"
#include "net/tel.h"
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
    bool last_rx_was_cr;
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

static void tel_send_cmd(int desc, uint8_t cmd, uint8_t opt)
{
    char buf[3] = {TEL_IAC, cmd, opt};
    net_tx(desc, buf, 3);
}

static void tel_handle_sb(int desc, tel_conn_t *tc)
{
    if (tc->sb_len < 1)
        return;
    if (tc->sb_buf[0] == TEL_OPT_TTYPE &&
        tc->sb_len >= 2 && tc->sb_buf[1] == TEL_TTYPE_SEND)
    {
        const char *ttype = mdm_settings->tty_type;
        size_t ttype_len = strlen(ttype);
        // IAC SB TTYPE IS <type> IAC SE
        char buf[48];
        size_t pos = 0;
        buf[pos++] = TEL_IAC;
        buf[pos++] = TEL_SB;
        buf[pos++] = TEL_OPT_TTYPE;
        buf[pos++] = TEL_TTYPE_IS;
        if (ttype_len > sizeof(buf) - pos - 2)
            ttype_len = sizeof(buf) - pos - 2;
        memcpy(&buf[pos], ttype, ttype_len);
        pos += ttype_len;
        buf[pos++] = TEL_IAC;
        buf[pos++] = TEL_SE;
        net_tx(desc, buf, pos);
        DBG("NET TEL sent TTYPE IS %s\n", ttype);
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
            tel_send_cmd(desc, TEL_DO, opt);
            DBG("NET TEL sent DO %d\n", opt);
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
        if (!(tc->opts_we_will & bit))
        {
            tc->opts_we_will |= bit;
            tel_send_cmd(desc, TEL_WILL, opt);
            DBG("NET TEL sent WILL %d\n", opt);
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
        // Strip NUL after CR in real telnet mode
        if (mdm_settings->net_mode == 1 && tc->last_rx_was_cr && byte == 0)
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

uint16_t tel_rx(int desc, char *buf, uint16_t len)
{
    if (mdm_settings->net_mode == 0)
        return net_rx(desc, buf, len);

    tel_conn_t *tc = &tel_conns[desc];
    uint16_t out = 0;
    while (out < len)
    {
        char byte;
        if (net_rx(desc, &byte, 1) == 0)
            break;
        tel_process_rx_byte(desc, tc, (uint8_t)byte, buf, &out, len);
    }
    return out;
}

uint16_t tel_tx(int desc, const char *buf, uint16_t len)
{
    if (mdm_settings->net_mode == 0)
        return net_tx(desc, buf, len);

    char out[128];
    uint16_t consumed = 0;
    uint16_t out_pos = 0;

    while (consumed < len)
    {
        uint8_t byte = buf[consumed];
        uint16_t needed = 1;
        if (byte == (uint8_t)TEL_IAC)
            needed = 2;
        else if (mdm_settings->net_mode == 1 && byte == '\r')
            needed = 2;

        if (out_pos + needed > sizeof(out))
            break;

        out[out_pos++] = byte;
        if (byte == (uint8_t)TEL_IAC)
            out[out_pos++] = (char)TEL_IAC;
        else if (mdm_settings->net_mode == 1 && byte == '\r')
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
            while (out_walk < sent && adj < consumed)
            {
                out_walk++;
                uint8_t b = buf[adj];
                if (b == (uint8_t)TEL_IAC ||
                    (mdm_settings->net_mode == 1 && b == '\r'))
                    out_walk++;
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

void tel_on_connect(int desc)
{
    if (mdm_settings->net_mode == 0)
        return;

    tel_conn_t *tc = &tel_conns[desc];
    tc->opts_we_will = TEL_BIT_BINARY | TEL_BIT_SGA | TEL_BIT_TTYPE;
    tc->opts_we_do = TEL_BIT_BINARY | TEL_BIT_SGA;

    char buf[] = {
        TEL_IAC,
        TEL_WILL,
        TEL_OPT_BINARY,
        TEL_IAC,
        TEL_DO,
        TEL_OPT_BINARY,
        TEL_IAC,
        TEL_WILL,
        TEL_OPT_SGA,
        TEL_IAC,
        TEL_DO,
        TEL_OPT_SGA,
        TEL_IAC,
        TEL_WILL,
        TEL_OPT_TTYPE,
    };
    net_tx(desc, buf, sizeof(buf));
    DBG("NET TEL sent initial negotiation\n");
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
    tel_on_connect(desc);
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

#endif /* RP6502_RIA_W */
