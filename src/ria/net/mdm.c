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

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_MDM)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...)
#endif

#define MDM_BUF_SIZE (TCP_MSS)
#define MDM_AT_COMMAND_LEN 255
static_assert(MDM_AT_COMMAND_LEN < MDM_BUF_SIZE);

static char mdm_buf[MDM_BUF_SIZE];
static size_t mdm_buf_len;

typedef enum
{
    mdm_at_state_start,
    mdm_at_state_char_a,
    mdm_at_state_char_t,
    mdm_at_state_reading,
} mdm_at_state_t;
static mdm_at_state_t mdm_at_state;
static bool mdm_is_open;
static bool mdm_in_command_mode;
static nvr_settings_t mdm_settings;

void modem_run(void); // TODO

void mdm_task()
{
    modem_run();
}

void mdm_stop(void)
{
    nvr_read(&mdm_settings);
    mdm_is_open = false;
    mdm_buf_len = 0;
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
    return true;
}

bool mdm_close(void)
{
    if (!mdm_is_open)
        return false;
    mdm_is_open = false;
    return true;
}

bool mdm_tx(char ch)
{
    if (!mdm_is_open)
        return false;
    if (mdm_in_command_mode)
    {
        switch (mdm_at_state)
        {
        case mdm_at_state_start:
            if (ch == mdm_settings.crChar)
            {
                mdm_at_state = mdm_at_state_char_a;
                mdm_buf_len = 0;
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
                if (mdm_buf_len == 0)
                    mdm_at_state = mdm_at_state_char_t;
                else
                    mdm_buf_len--;
                break;
            }
            else if (ch == mdm_settings.crChar)
            {
                mdm_at_state = mdm_at_state_start;
                mdm_buf[mdm_buf_len] = 0;
                // TODO process command
            }
            else if (mdm_buf_len < MDM_AT_COMMAND_LEN)
            {
                mdm_buf[mdm_buf_len++] = ch;
            }
            break;
        }
        if (mdm_settings.echo)
        {
            if (ch == mdm_settings.bsChar)
            {
                // TODO
            }
            else
            {
                // TODO
            }
        }
        return true;
    }
    else
    {
        if (mdm_buf_len >= MDM_BUF_SIZE)
            return false;
        mdm_buf[mdm_buf_len++] = ch;
        return true;
    }
}
