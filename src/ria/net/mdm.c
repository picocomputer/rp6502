/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"
#include "lwipopts.h"
#include "str.h"
#include "net/cmd.h"
#include "net/mdm.h"
#include "net/nvr.h"
#include "net/wfi.h"
#include <string.h>
#include <stdio.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_MDM)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...)
#endif

#define MDM_TX_BUF_SIZE (TCP_MSS)
#define MDM_AT_COMMAND_LEN 255
static_assert(MDM_AT_COMMAND_LEN < MDM_TX_BUF_SIZE);

static char mdm_tx_buf[MDM_TX_BUF_SIZE];
static size_t mdm_tx_buf_len;

#define MDM_RX_BUF_SIZE (128)

static char mdm_rx_buf[MDM_RX_BUF_SIZE];
static size_t mdm_rx_buf_head;
static size_t mdm_rx_buf_tail;

static int (*mdm_rx_callback_fn)(char *, size_t, int);
static int mdm_rx_callback_state;

typedef enum
{
    mdm_state_command_mode,
    mdm_state_parsing,
    mdm_state_connecting,
    mdm_state_connected,
} mdm_state_t;
static mdm_state_t mdm_state;
static const char *mdm_parse_str;
static bool mdm_parse_result;
static bool mdm_is_open;

nvr_settings_t mdm_settings;

static const char __in_flash("net_mdm") str0[] = "OK";
static const char __in_flash("net_mdm") str1[] = "CONNECT";
static const char __in_flash("net_mdm") str2[] = "RING";
static const char __in_flash("net_mdm") str3[] = "NO CARRIER";
static const char __in_flash("net_mdm") str4[] = "ERROR";
static const char __in_flash("net_mdm") str5[] = "CONNECT 1200";
static const char __in_flash("net_mdm") str6[] = "NO DIALTONE";
static const char __in_flash("net_mdm") str7[] = "BUSY";
static const char __in_flash("net_mdm") str8[] = "NO ANSWER";
static const char *const __in_flash("net_mdm") mdm_response_strings[] = {str0, str1, str2, str3, str4, str5, str6, str7, str8};

void mdm_stop(void)
{
    mdm_is_open = false;
    mdm_tx_buf_len = 0;
    mdm_rx_buf_head = 0;
    mdm_rx_buf_tail = 0;
    mdm_rx_callback_state = -1;
    mdm_parse_result = true;
    mdm_state = mdm_state_command_mode;
}

void mdm_init(void)
{
    mdm_stop();
}

bool mdm_open(const char *filename)
{
    if (mdm_is_open)
        return false;
    if (!strncasecmp(filename, "AT:", 3))
        filename += 3;
    else if (!strncasecmp(filename, "AT0:", 4))
        filename += 4;
    else
        return false;
    nvr_read(&mdm_settings);
    mdm_is_open = true;
    // optionally process filename as AT command
    // after nvram read. e.g. AT:&F
    if (filename[0])
    {
        mdm_state = mdm_state_parsing;
        mdm_parse_result = true;
        mdm_parse_str = filename;
    }
    return true;
}

bool mdm_close(void)
{
    if (!mdm_is_open)
        return false;
    mdm_stop();
    return true;
}

static inline bool mdm_rx_buf_empty(void)
{
    return mdm_rx_buf_head == mdm_rx_buf_tail;
}

static inline bool mdm_rx_buf_full(void)
{
    return ((mdm_rx_buf_head + 1) % MDM_RX_BUF_SIZE) == mdm_rx_buf_tail;
}

static inline size_t mdm_rx_buf_count(void)
{
    if (mdm_rx_buf_head >= mdm_rx_buf_tail)
        return mdm_rx_buf_head - mdm_rx_buf_tail;
    else
        return MDM_RX_BUF_SIZE - mdm_rx_buf_tail + mdm_rx_buf_head;
}

void mdm_set_response_fn(int (*fn)(char *, size_t, int), int state)
{
    assert(mdm_rx_callback_state == -1);
    mdm_rx_callback_fn = fn;
    mdm_rx_callback_state = state;
}

static void mdm_response_append(char ch)
{
    if (!mdm_rx_buf_full())
    {
        mdm_rx_buf[mdm_rx_buf_head] = ch;
        mdm_rx_buf_head = (mdm_rx_buf_head + 1) % MDM_RX_BUF_SIZE;
    }
}

static void mdm_response_append_cr_lf(void)
{
    if (!(mdm_settings.cr_char & 0x80))
        mdm_response_append(mdm_settings.cr_char);
    if (!(mdm_settings.lf_char & 0x80))
        mdm_response_append(mdm_settings.lf_char);
}

int mdm_rx(char *ch)
{
    if (!mdm_is_open)
        return -1;
    // get next line, if needed and in progress
    if (mdm_rx_buf_empty() && mdm_rx_callback_state >= 0)
    {
        mdm_rx_callback_state = mdm_rx_callback_fn(mdm_rx_buf, MDM_RX_BUF_SIZE, mdm_rx_callback_state);
        mdm_rx_buf_head = strlen(mdm_rx_buf);
        mdm_rx_buf_tail = 0;
        // Translate CR and LF chars to settings
        for (size_t i = 0; i < mdm_rx_buf_head; i++)
        {
            uint8_t swap_ch = 0;
            if (mdm_rx_buf[i] == '\r')
                swap_ch = mdm_rx_buf[i] = mdm_settings.cr_char;
            if (mdm_rx_buf[i] == '\n')
                swap_ch = mdm_rx_buf[i] = mdm_settings.lf_char;
            if (swap_ch & 0x80)
            {
                for (size_t j = i; j < mdm_rx_buf_head; j++)
                    mdm_rx_buf[j] = mdm_rx_buf[j + 1];
                mdm_rx_buf_head--;
            }
        }
    }
    // get from line buffer, if available
    if (!mdm_rx_buf_empty())
    {
        *ch = mdm_rx_buf[mdm_rx_buf_tail];
        mdm_rx_buf_tail = (mdm_rx_buf_tail + 1) % MDM_RX_BUF_SIZE;
        return 1;
    }
    // get from telnet filter, which gets from pbuf
    // TODO
    return 0;
}

static int mdm_tx_command_mode(char ch)
{
    if (mdm_rx_callback_state >= 0)
        return 0;
    if (ch == '\r' || (!(mdm_settings.cr_char & 0x80) && ch == mdm_settings.cr_char))
    {
        if (mdm_settings.echo)
            mdm_response_append_cr_lf();
        mdm_tx_buf[mdm_tx_buf_len] = 0;
        mdm_tx_buf_len = 0;
        if ((mdm_tx_buf[0] == 'a' || mdm_tx_buf[0] == 'A') &&
            (mdm_tx_buf[1] == 't' || mdm_tx_buf[1] == 'T'))
        {
            if (!mdm_settings.echo && !mdm_settings.quiet && mdm_settings.verbose)
                mdm_response_append_cr_lf();
            mdm_state = mdm_state_parsing;
            mdm_parse_result = true;
            mdm_parse_str = &mdm_tx_buf[2];
        }
    }
    else if (ch == 127 || (!(mdm_settings.bs_char & 0x80) && ch == mdm_settings.bs_char))
    {
        if (mdm_settings.echo)
        {
            mdm_response_append(mdm_settings.bs_char);
            mdm_response_append(' ');
            mdm_response_append(mdm_settings.bs_char);
        }
        if (mdm_tx_buf_len)
            mdm_tx_buf[--mdm_tx_buf_len] = 0;
    }
    else if (ch >= 32 && ch < 127)
    {
        if (mdm_settings.echo)
            mdm_response_append(ch);
        if (ch == '/' && mdm_tx_buf_len == 1 &&
            (mdm_tx_buf[0] == 'a' || mdm_tx_buf[0] == 'A') &&
            (mdm_tx_buf[1] == 't' || mdm_tx_buf[1] == 'T'))
        {
            if (mdm_settings.echo || (!mdm_settings.quiet && mdm_settings.verbose))
                mdm_response_append_cr_lf();
            mdm_tx_buf_len = 0;
            mdm_state = mdm_state_parsing;
            mdm_parse_result = true;
            mdm_parse_str = &mdm_tx_buf[2];
            return 1;
        }
        if (mdm_tx_buf_len < MDM_TX_BUF_SIZE - 1)
            mdm_tx_buf[mdm_tx_buf_len++] = ch;
    }
    return 1;
}

static int mdm_tx_connected(char ch)
{
    if (mdm_tx_buf_len >= MDM_TX_BUF_SIZE)
        return 0;
    mdm_tx_buf[mdm_tx_buf_len++] = ch;
    return 1;
}

int mdm_tx(char ch)
{
    if (!mdm_is_open)
        return -1;
    if (mdm_state == mdm_state_command_mode)
        return mdm_tx_command_mode(ch);
    if (mdm_state == mdm_state_connected)
        return mdm_tx_connected(ch);
    return 0;
}

int mdm_response_code(char *buf, size_t buf_size, int state)
{
    assert(state >= 0 && (unsigned)state < sizeof(mdm_response_strings) / sizeof(char *));
    if (mdm_settings.quiet == 2 ||
        (mdm_settings.quiet == 1 && state != 1 && state != 2 && state != 3))
        buf[0] = 0;
    else if (mdm_settings.verbose)
        snprintf(buf, buf_size, "%s\r\n", mdm_response_strings[state]);
    else
        snprintf(buf, buf_size, "%d\r", state);
    return -1;
}

void mdm_task()
{
    if (mdm_state == mdm_state_parsing)
    {
        if (mdm_rx_callback_state >= 0)
            return;
        if (!mdm_parse_result)
        {
            mdm_set_response_fn(mdm_response_code, 4); // ERROR
            mdm_state = mdm_state_command_mode;
        }
        else if (*mdm_parse_str == 0)
        {
            mdm_set_response_fn(mdm_response_code, 0); // OK
            mdm_state = mdm_state_command_mode;
        }
        else
        {
            mdm_parse_result = cmd_parse(&mdm_parse_str);
        }
    }
    if (mdm_state == mdm_state_connected)
    {
        // TODO
    }
}
