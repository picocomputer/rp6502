/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "str.h"
#include "net/cmd.h"
#include "net/mdm.h"
#include "net/nvr.h"
#include <stdio.h>
#include <ctype.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_NVR)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...)
#endif

static int cmd_view_config_response(char *buf, size_t buf_size, int state)
{
    switch (state)
    {
    case 0:
        snprintf(buf, buf_size, "line 0\r\n");
        break;
    case 1:
        snprintf(buf, buf_size, "line 1\r\n");
        break;
    case 2:
        snprintf(buf, buf_size, "line 2\r\n");
        __attribute__((fallthrough));
    default:
        return -1;
    }
    return ++state;
}

static bool cmd_view_config(const char **s)
{
    switch (**s)
    {
    case '0':
        ++*s;
        __attribute__((fallthrough));
    case '\0':
        mdm_set_response_fn(cmd_view_config_response, 0);
        break;
    case '1':
        ++*s;
        // TODO
        return false;
        break;
    }
    return true;
}

static bool cmd_parse_amp(const char **s)
{
    if (toupper(**s) == 'V')
    {
        (*s)++;
        return cmd_view_config(s);
    }
    return false;
}

bool cmd_parse(const char **s)
{
    if (**s == '&')
    {
        (*s)++;
        return cmd_parse_amp(s);
    }
    return false;
}
