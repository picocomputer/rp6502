/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"
#include "lwipopts.h"
#include "str.h"
#include "net/mdm.h"
#include "net/nvr.h"
#include "net/wfi.h"
#include <string.h>

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

static int mdm_rx_callback_state;
static int (*mdm_rx_callback_fn)(char *, size_t, int);

typedef enum
{
    mdm_state_command_mode,
    mdm_state_parsing,
    mdm_state_connecting,
    mdm_state_connected,
} mdm_state_t;
static mdm_state_t mdm_state;

typedef enum
{
    mdm_at_state_start,
    mdm_at_state_char_a,
    mdm_at_state_char_t,
    mdm_at_state_reading,
} mdm_at_state_t;
static mdm_at_state_t mdm_at_state;
static bool mdm_is_open;
static bool mdm_was_opened;
static bool mdm_in_command_mode;
static nvr_settings_t mdm_settings;

void modem_run(void); // TODO

void mdm_task()
{
    modem_run();
}

void mdm_stop(void)
{
    if (!mdm_was_opened)
        return;
    nvr_read(&mdm_settings);
    mdm_is_open = false;
    mdm_was_opened = false;
    mdm_tx_buf_len = 0;
    mdm_rx_buf_head = 0;
    mdm_rx_buf_tail = 0;
    mdm_rx_callback_state = 0;
    mdm_in_command_mode = true;
}

void mdm_init(void)
{
    mdm_stop();
}

bool mdm_open(const char *filename)
{
    if (mdm_is_open)
        return false;
    while (*filename == ' ')
        filename++;
    if (!strnicmp(filename, "AT:", 3))
        filename += 3;
    else if (!strnicmp(filename, "AT0:", 4))
        filename += 4;
    else
        return false;
    // TODO populate command buffer with filename
    mdm_is_open = true;
    mdm_was_opened = true;
    return true;
}

bool mdm_close(void)
{
    if (!mdm_is_open)
        return false;
    mdm_is_open = false;
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
    assert(mdm_rx_callback_state == 0);
    mdm_rx_callback_state = state;
    mdm_rx_callback_fn = fn;
}

void mdm_response_append(char ch)
{
    if (!mdm_rx_buf_full())
    {
        mdm_rx_buf_head = (mdm_rx_buf_head + 1) % MDM_RX_BUF_SIZE;
        mdm_rx_buf[mdm_rx_buf_head] = ch;
    }
}

int mdm_rx(char *ch)
{
    if (!mdm_is_open)
        return -1;
    // get next line, if needed and in progress
    if (mdm_rx_buf_empty() && mdm_rx_callback_state)
    {
        mdm_rx_callback_state = mdm_rx_callback_fn(mdm_rx_buf, MDM_RX_BUF_SIZE, mdm_rx_callback_state);
        mdm_rx_buf_head = 0;
        mdm_rx_buf_tail = strlen(mdm_rx_buf);
        for (size_t i = 0; i < mdm_rx_buf_tail; i++)
        {
            if (mdm_rx_buf[i] == '\r')
                mdm_rx_buf[i] = mdm_settings.crChar;
            if (mdm_rx_buf[i] == '\n')
                mdm_rx_buf[i] = mdm_settings.lfChar;
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

int mdm_tx(char ch)
{
    if (!mdm_is_open)
        return -1;
    if (mdm_in_command_mode)
    {
        if (mdm_rx_callback_state)
            return 0;
        switch (mdm_at_state)
        {
        case mdm_at_state_start:
            if (ch == mdm_settings.crChar)
            {
                mdm_at_state = mdm_at_state_char_a;
                mdm_tx_buf_len = 0;
            }
            break;
        case mdm_at_state_char_a:
            if (ch == 'a' || ch == 'A')
                mdm_at_state = mdm_at_state_char_t;
            else
                mdm_at_state = mdm_at_state_start;
            break;
        case mdm_at_state_char_t:
            if (ch == mdm_settings.bsChar)
                mdm_at_state = mdm_at_state_char_a;
            else if (ch == 't' || ch == 'T')
                mdm_at_state = mdm_at_state_reading;
            else
                mdm_at_state = mdm_at_state_start;
            break;
        case mdm_at_state_reading:
            if (ch == mdm_settings.bsChar)
            {
                if (mdm_tx_buf_len == 0)
                    mdm_at_state = mdm_at_state_char_t;
                else
                    mdm_tx_buf_len--;
                break;
            }
            else if (ch == mdm_settings.crChar)
            {
                mdm_at_state = mdm_at_state_start;
                mdm_tx_buf[mdm_tx_buf_len] = 0;
                // TODO process command
            }
            else if (mdm_tx_buf_len < MDM_AT_COMMAND_LEN)
            {
                mdm_tx_buf[mdm_tx_buf_len++] = ch;
            }
            break;
        }
        if (mdm_settings.echo)
        {
            if (ch == mdm_settings.crChar)
            {
                mdm_response_append(mdm_settings.crChar);
                mdm_response_append(mdm_settings.lfChar);
            }
            else if (ch == mdm_settings.bsChar)
            {
                mdm_response_append(ch);
                mdm_response_append(' ');
                mdm_response_append(ch);
            }
            else
                mdm_response_append(ch);
        }
        return 1;
    }
    else
    {
        if (mdm_tx_buf_len >= MDM_TX_BUF_SIZE)
            return 0;
        mdm_tx_buf[mdm_tx_buf_len++] = ch;
        return 1;
    }
}
