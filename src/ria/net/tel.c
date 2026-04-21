/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RP6502_RIA_W

#include "main.h"
#include "net/mdm.h"
#include "net/net.h"
#include "net/tel.h"
#include <pico/stdlib.h>
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
#define TEL_BRK 243

// Telnet option codes
#define TEL_OPT_BINARY 0
#define TEL_OPT_ECHO 1
#define TEL_OPT_SGA 3
#define TEL_OPT_TTYPE 24

// Subnegotiation commands
#define TEL_TTYPE_IS 0
#define TEL_TTYPE_SEND 1

// Tracked options — indexed into us[]/him[] state arrays.
enum
{
    TEL_IDX_BINARY,
    TEL_IDX_ECHO,
    TEL_IDX_SGA,
    TEL_IDX_TTYPE,
    TEL_OPT_COUNT,
};
static const uint8_t tel_opt_codes[TEL_OPT_COUNT] = {
    TEL_OPT_BINARY, TEL_OPT_ECHO, TEL_OPT_SGA, TEL_OPT_TTYPE};

// RFC 1143 Q-method per-option states.
typedef enum
{
    tel_q_no,
    tel_q_yes,
    tel_q_wantno,
    tel_q_wantno_op,
    tel_q_wantyes,
    tel_q_wantyes_op,
} tel_q_t;

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
    uint8_t us[TEL_OPT_COUNT];  // our side (WILL/WONT)
    uint8_t him[TEL_OPT_COUNT]; // remote side (DO/DONT)
    // Retry bitmaps — one bit per option, at most one of the four set per opt.
    uint8_t pending_will;
    uint8_t pending_wont;
    uint8_t pending_do;
    uint8_t pending_dont;
    bool last_rx_was_cr;
    bool tx_cr_pending;
    bool telnet_mode;
    bool is_server;
    bool tx_escape_pending;
    uint8_t tx_escape_byte;
    const char *ttype;
} tel_conn_t;

static tel_conn_t tel_conns[NET_MAX_CONNECTIONS];

static int tel_opt_idx(uint8_t code)
{
    for (int i = 0; i < TEL_OPT_COUNT; i++)
        if (tel_opt_codes[i] == code)
            return i;
    return -1;
}

// Policy: which options we accept when the peer asks us to enable.
static bool tel_accept_us(const tel_conn_t *tc, int idx)
{
    if (idx == TEL_IDX_ECHO)
        return tc->is_server; // terminal must not echo; only server echoes
    return idx == TEL_IDX_BINARY || idx == TEL_IDX_SGA || idx == TEL_IDX_TTYPE;
}

// Policy: which options we accept when the peer offers to enable.
static bool tel_accept_him(const tel_conn_t *tc, int idx)
{
    (void)tc;
    // Peer TTYPE is never offered via WILL (peer sends WILL TTYPE only in
    // response to our DO TTYPE). Accept the rest.
    return idx == TEL_IDX_BINARY || idx == TEL_IDX_ECHO || idx == TEL_IDX_SGA;
}

static bool tel_raw_send(int desc, uint8_t cmd, uint8_t opt)
{
    char buf[3] = {(char)TEL_IAC, (char)cmd, (char)opt};
    return net_tx_all(desc, buf, 3);
}

// Atomic IAC triple send. Clears any earlier pending command for this option,
// then sets the appropriate pending bit on failure.
static void tel_q_send(int desc, tel_conn_t *tc, uint8_t cmd, int idx)
{
    uint8_t bit = (uint8_t)(1u << idx);
    tc->pending_will &= ~bit;
    tc->pending_wont &= ~bit;
    tc->pending_do &= ~bit;
    tc->pending_dont &= ~bit;
    if (tel_raw_send(desc, cmd, tel_opt_codes[idx]))
        return;
    switch (cmd)
    {
    case TEL_WILL:
        tc->pending_will |= bit;
        break;
    case TEL_WONT:
        tc->pending_wont |= bit;
        break;
    case TEL_DO:
        tc->pending_do |= bit;
        break;
    case TEL_DONT:
        tc->pending_dont |= bit;
        break;
    }
}

// Retry any commands whose previous atomic send failed.
static void tel_q_flush(int desc, tel_conn_t *tc)
{
    uint8_t any = tc->pending_will | tc->pending_wont |
                  tc->pending_do | tc->pending_dont;
    if (!any)
        return;
    for (int i = 0; i < TEL_OPT_COUNT; i++)
    {
        uint8_t bit = (uint8_t)(1u << i);
        if (tc->pending_will & bit)
            tel_q_send(desc, tc, TEL_WILL, i);
        else if (tc->pending_wont & bit)
            tel_q_send(desc, tc, TEL_WONT, i);
        else if (tc->pending_do & bit)
            tel_q_send(desc, tc, TEL_DO, i);
        else if (tc->pending_dont & bit)
            tel_q_send(desc, tc, TEL_DONT, i);
    }
}

// RFC 1143: peer sent WILL opt — drives our view of him[].
static void tel_q_recv_will(int desc, tel_conn_t *tc, int idx)
{
    switch (tc->him[idx])
    {
    case tel_q_no:
        if (tel_accept_him(tc, idx))
        {
            tc->him[idx] = tel_q_yes;
            tel_q_send(desc, tc, TEL_DO, idx);
        }
        else
        {
            tel_q_send(desc, tc, TEL_DONT, idx);
        }
        break;
    case tel_q_yes:
        break;
    case tel_q_wantno:
        tc->him[idx] = tel_q_no; // error: DONT answered by WILL
        break;
    case tel_q_wantno_op:
        tc->him[idx] = tel_q_yes; // error: DONT answered by WILL
        break;
    case tel_q_wantyes:
        tc->him[idx] = tel_q_yes;
        break;
    case tel_q_wantyes_op:
        tc->him[idx] = tel_q_wantno;
        tel_q_send(desc, tc, TEL_DONT, idx);
        break;
    }
}

// RFC 1143: peer sent WONT opt.
static void tel_q_recv_wont(int desc, tel_conn_t *tc, int idx)
{
    switch (tc->him[idx])
    {
    case tel_q_no:
        break;
    case tel_q_yes:
        tc->him[idx] = tel_q_no;
        tel_q_send(desc, tc, TEL_DONT, idx);
        break;
    case tel_q_wantno:
        tc->him[idx] = tel_q_no;
        break;
    case tel_q_wantno_op:
        tc->him[idx] = tel_q_wantyes;
        tel_q_send(desc, tc, TEL_DO, idx);
        break;
    case tel_q_wantyes:
    case tel_q_wantyes_op:
        tc->him[idx] = tel_q_no;
        break;
    }
}

// RFC 1143: peer sent DO opt — drives us[].
static void tel_q_recv_do(int desc, tel_conn_t *tc, int idx)
{
    switch (tc->us[idx])
    {
    case tel_q_no:
        if (tel_accept_us(tc, idx))
        {
            tc->us[idx] = tel_q_yes;
            tel_q_send(desc, tc, TEL_WILL, idx);
        }
        else
        {
            tel_q_send(desc, tc, TEL_WONT, idx);
        }
        break;
    case tel_q_yes:
        break;
    case tel_q_wantno:
        tc->us[idx] = tel_q_no; // error: WONT answered by DO
        break;
    case tel_q_wantno_op:
        tc->us[idx] = tel_q_yes; // error
        break;
    case tel_q_wantyes:
        tc->us[idx] = tel_q_yes;
        break;
    case tel_q_wantyes_op:
        tc->us[idx] = tel_q_wantno;
        tel_q_send(desc, tc, TEL_WONT, idx);
        break;
    }
}

// RFC 1143: peer sent DONT opt.
static void tel_q_recv_dont(int desc, tel_conn_t *tc, int idx)
{
    switch (tc->us[idx])
    {
    case tel_q_no:
        break;
    case tel_q_yes:
        tc->us[idx] = tel_q_no;
        tel_q_send(desc, tc, TEL_WONT, idx);
        break;
    case tel_q_wantno:
        tc->us[idx] = tel_q_no;
        break;
    case tel_q_wantno_op:
        tc->us[idx] = tel_q_wantyes;
        tel_q_send(desc, tc, TEL_WILL, idx);
        break;
    case tel_q_wantyes:
    case tel_q_wantyes_op:
        tc->us[idx] = tel_q_no;
        break;
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
        buf[pos++] = (char)TEL_IAC;
        buf[pos++] = (char)TEL_SB;
        buf[pos++] = (char)TEL_OPT_TTYPE;
        buf[pos++] = (char)TEL_TTYPE_IS;
        if (ttype_len > sizeof(buf) - pos - 2)
            ttype_len = sizeof(buf) - pos - 2;
        memcpy(&buf[pos], tc->ttype, ttype_len);
        pos += ttype_len;
        buf[pos++] = (char)TEL_IAC;
        buf[pos++] = (char)TEL_SE;
        // Atomic: on failure the peer can resend TTYPE SEND.
        if (net_tx_all(desc, buf, pos))
            DBG("NET TEL sent TTYPE IS %s\n", tc->ttype);
    }
}

// Dispatch one received negotiation byte. idx<0 for unknown options: respond
// with DONT/WONT without tracking state (peer can resend if lost).
static void tel_dispatch_neg(int desc, tel_conn_t *tc, uint8_t cmd, uint8_t opt)
{
    int idx = tel_opt_idx(opt);
    if (idx < 0)
    {
        if (cmd == TEL_WILL)
            tel_raw_send(desc, TEL_DONT, opt);
        else if (cmd == TEL_DO)
            tel_raw_send(desc, TEL_WONT, opt);
        return;
    }
    switch (cmd)
    {
    case TEL_WILL:
        tel_q_recv_will(desc, tc, idx);
        break;
    case TEL_WONT:
        tel_q_recv_wont(desc, tc, idx);
        break;
    case TEL_DO:
        tel_q_recv_do(desc, tc, idx);
        break;
    case TEL_DONT:
        tel_q_recv_dont(desc, tc, idx);
        break;
    }
}

static void tel_process_rx_byte(int desc, tel_conn_t *tc, uint8_t byte,
                                char *buf, uint16_t *out_pos, uint16_t cap)
{
    switch (tc->rx_state)
    {
    case tel_rx_data:
        if (byte == TEL_IAC)
        {
            tc->rx_state = tel_rx_iac;
            return;
        }
        // Strip NUL after CR in NVT mode (binary mode sends raw)
        if (tc->telnet_mode && tc->him[TEL_IDX_BINARY] != tel_q_yes &&
            tc->last_rx_was_cr && byte == 0)
        {
            tc->last_rx_was_cr = false;
            return;
        }
        tc->last_rx_was_cr = (byte == '\r');
        if (*out_pos < cap)
            buf[(*out_pos)++] = byte;
        return;

    case tel_rx_iac:
        switch (byte)
        {
        case TEL_IAC:
            // Escaped 0xFF - literal data byte
            tc->rx_state = tel_rx_data;
            if (*out_pos < cap)
                buf[(*out_pos)++] = (char)0xFF;
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
        case TEL_BRK:
            if (tc->is_server)
                main_break();
            tc->rx_state = tel_rx_data;
            return;
        default:
            // NOP or other command - ignore
            tc->rx_state = tel_rx_data;
            return;
        }

    case tel_rx_will:
        tel_dispatch_neg(desc, tc, TEL_WILL, byte);
        tc->rx_state = tel_rx_data;
        return;

    case tel_rx_wont:
        tel_dispatch_neg(desc, tc, TEL_WONT, byte);
        tc->rx_state = tel_rx_data;
        return;

    case tel_rx_do:
        tel_dispatch_neg(desc, tc, TEL_DO, byte);
        tc->rx_state = tel_rx_data;
        return;

    case tel_rx_dont:
        tel_dispatch_neg(desc, tc, TEL_DONT, byte);
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
        // Malformed IAC inside SB — abandon subneg
        tc->rx_state = tel_rx_data;
        return;
    }
}

// -- Telnet protocol API --

// Decode up to `cap` bytes from the wire into `out`, driving the IAC state
// machine. Shared by tel_rx and the console-server drain path. Also opportunistic-
// ally retries any failed-to-send negotiation commands.
static uint16_t tel_decode(int desc, char *out, uint16_t cap)
{
    tel_conn_t *tc = &tel_conns[desc];
    tel_q_flush(desc, tc);
    uint16_t out_pos = 0;
    while (out_pos < cap)
    {
        char raw[64];
        uint16_t want = cap - out_pos;
        if (want > sizeof(raw))
            want = sizeof(raw);
        uint16_t n = net_rx(desc, raw, want);
        if (n == 0)
            break;
        for (uint16_t i = 0; i < n; i++)
            tel_process_rx_byte(desc, tc, (uint8_t)raw[i], out, &out_pos, cap);
    }
    return out_pos;
}

uint16_t tel_rx(int desc, char *buf, uint16_t len)
{
    tel_conn_t *tc = &tel_conns[desc];
    if (!tc->telnet_mode)
        return net_rx(desc, buf, len);
    return tel_decode(desc, buf, len);
}

// Compute the wire size of a source byte during NVT TX encoding.
// For CR in NVT mode, caller passes the next source byte (or -1 at end
// of buffer) so we can distinguish CR-LF (1 byte) from bare CR-NUL (2).
static uint16_t tel_tx_step(uint8_t byte, bool binary_tx, int next)
{
    if (byte == (uint8_t)TEL_IAC)
        return 2;
    if (byte == '\r' && !binary_tx && next >= 0 && next != '\n')
        return 2; // bare CR -> CR NUL
    // next==-1 (end of buffer) counts as 1: a companion NUL, if needed,
    // is accounted for against the next call's first byte.
    return 1;
}

uint16_t tel_tx(int desc, const char *buf, uint16_t len)
{
    tel_conn_t *tc = &tel_conns[desc];
    if (!tc->telnet_mode)
        return net_tx(desc, buf, len);

    tel_q_flush(desc, tc);

    // Flush the second byte of a split escape sequence
    if (tc->tx_escape_pending)
    {
        char b = tc->tx_escape_byte;
        if (net_tx(desc, &b, 1) != 1)
            return 0;
        tc->tx_escape_pending = false;
    }

    // Handle deferred CR-NUL from previous call
    if (tc->tx_cr_pending)
    {
        tc->tx_cr_pending = false;
        if (len == 0 || buf[0] != '\n')
        {
            // Bare CR — send the deferred NUL
            char nul = 0;
            if (net_tx(desc, &nul, 1) != 1)
            {
                tc->tx_cr_pending = true;
                return 0;
            }
        }
    }

    bool binary_tx = tc->us[TEL_IDX_BINARY] == tel_q_yes;
    char out[128];
    uint16_t consumed = 0;
    uint16_t out_pos = 0;

    while (consumed < len)
    {
        uint8_t byte = buf[consumed];
        int next = (consumed + 1 < len) ? (uint8_t)buf[consumed + 1] : -1;
        uint16_t needed = tel_tx_step(byte, binary_tx, next);

        if (out_pos + needed > sizeof(out))
            break;

        out[out_pos++] = byte;
        if (byte == (uint8_t)TEL_IAC)
            out[out_pos++] = (char)TEL_IAC;
        else if (byte == '\r' && !binary_tx)
        {
            if (next == -1)
                tc->tx_cr_pending = true; // defer until next call
            else if (next != '\n')
                out[out_pos++] = 0; // bare CR -> CR NUL
        }
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
                int nx = (adj + 1 < len) ? (uint8_t)buf[adj + 1] : -1;
                uint16_t step = tel_tx_step(buf[adj], binary_tx, nx);
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
            // Roll back deferred CR if it wasn't actually sent
            if (tc->tx_cr_pending && adj < consumed)
                tc->tx_cr_pending = false;
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

// Local-initiated: ask to enable our side of this option.
static void tel_q_ask_us_enable(int desc, tel_conn_t *tc, int idx)
{
    if (tc->us[idx] == tel_q_no)
    {
        tc->us[idx] = tel_q_wantyes;
        tel_q_send(desc, tc, TEL_WILL, idx);
    }
}

// Local-initiated: ask to enable peer side of this option.
static void tel_q_ask_him_enable(int desc, tel_conn_t *tc, int idx)
{
    if (tc->him[idx] == tel_q_no)
    {
        tc->him[idx] = tel_q_wantyes;
        tel_q_send(desc, tc, TEL_DO, idx);
    }
}

void tel_negotiate(int desc)
{
    tel_conn_t *tc = &tel_conns[desc];
    tc->telnet_mode = (mdm_settings->net_mode != 0);
    if (!tc->telnet_mode)
        return;

    tc->is_server = false;
    tc->ttype = mdm_settings->tty_type;
    // Order matches the pre-refactor code: DO then WILL per option. Some
    // peers (Synchronet) key initial behavior off the sequence.
    tel_q_ask_him_enable(desc, tc, TEL_IDX_BINARY); // DO BINARY
    tel_q_ask_us_enable(desc, tc, TEL_IDX_BINARY);  // WILL BINARY
    tel_q_ask_him_enable(desc, tc, TEL_IDX_SGA);    // DO SGA
    tel_q_ask_us_enable(desc, tc, TEL_IDX_SGA);     // WILL SGA
    tel_q_ask_us_enable(desc, tc, TEL_IDX_TTYPE);   // WILL TTYPE
    DBG("NET TEL sent initial negotiation\n");
}

static void tel_negotiate_server(int desc)
{
    tel_conn_t *tc = &tel_conns[desc];
    tc->telnet_mode = true;
    tc->is_server = true;
    tc->ttype = NULL;
    tel_q_ask_him_enable(desc, tc, TEL_IDX_SGA); // DO SGA
    tel_q_ask_us_enable(desc, tc, TEL_IDX_ECHO); // WILL ECHO
    tel_q_ask_us_enable(desc, tc, TEL_IDX_SGA);  // WILL SGA
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
    // Modem-emulator role: the retro machine is always the terminal, even
    // when answering a call. Use client-side negotiation.
    tel_reset(desc);
    if (!net_accept(desc, port, on_close))
        return false;
    tel_negotiate(desc);
    return true;
}

// Sibling to tel_accept, but with server-side negotiation — the retro
// machine is hosting (console server role).
bool tel_accept_server(int desc, uint16_t port, void (*on_close)(int))
{
    tel_reset(desc);
    if (!net_accept(desc, port, on_close))
        return false;
    tel_negotiate_server(desc);
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
