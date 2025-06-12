/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"
#include "lwipopts.h"
#include "str.h"
#include "net/mdm.h"
#include "net/wfi.h"

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_MDM)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...)
#endif

#define MDM_BUF_SIZE (TCP_MSS)

typedef enum
{
    mdm_at_state_start,
    mdm_at_state_char_a,
    mdm_at_state_char_t,
    mdm_at_state_reading,
} mdm_at_state_t;
static mdm_at_state_t mdm_at_state;

static char mdm_buf[MDM_BUF_SIZE];
static size_t mdm_buf_len;
static bool mdm_is_open;
static bool mdm_in_command_mode;

void modem_run(void); // TODO

void mdm_task()
{
    modem_run();
}

void mdm_reset(void)
{
    mdm_is_open = false;
    mdm_buf_len = 0;
    mdm_in_command_mode = true;
}

void mdm_init(void)
{
    mdm_reset();
}

bool mdm_open(const char *filename)
{
    if (mdm_is_open)
        return false;
    while (*filename == ' ')
        filename++;
    if (!strnicmp(filename, "MODEM:", 6))
        filename += 6;
    else if (!strnicmp(filename, "AT:", 3))
        filename += 3;
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
    if (mdm_in_command_mode)
    {
    }
    else
    {
        if (mdm_buf_len >= MDM_BUF_SIZE)
            return false;
        mdm_buf[mdm_buf_len++] = ch;
        return true;
    }
}
