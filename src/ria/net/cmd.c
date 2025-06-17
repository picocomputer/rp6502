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

// returns parsed number or -1 if no number
static int cmd_parse_num(const char **s)
{
    int num = -1;
    while ((**s >= '0') && (**s <= '9'))
    {
        if (num < 0)
            num = 0;
        num = num * 10 + (**s - '0');
        ++*s;
    }
    return num;
}

// E0, E1
static bool cmd_echo(const char **s)
{
    switch (cmd_parse_num(s))
    {
    case 0:
        mdm_settings.echo = 0;
        return true;
    case 1:
        mdm_settings.echo = 1;
        return true;
    }
    return false;
}

// V0, V1
static bool cmd_verbose(const char **s)
{
    switch (cmd_parse_num(s))
    {
    case 0:
        mdm_settings.verbose = 0;
        return true;
    case 1:
        mdm_settings.verbose = 1;
        return true;
    }
    return false;
}

// &F
static bool cmd_load_factory(const char **s)
{
    switch (cmd_parse_num(s))
    {
    case -1:
        nvr_factory_reset(&mdm_settings);
        return true;
    }
    return false;
}

static int cmd_view_config_response(char *buf, size_t buf_size, int state)
{
    nvr_settings_t nvr_settings;
    if (state >= 4)
        nvr_read(&nvr_settings);

    switch (state)
    {
    case 0:
        snprintf(buf, buf_size, "ACTIVE PROFILE:\r\n");
        break;
    case 1:
        snprintf(buf, buf_size, "E%u V%u\r\n",
                 mdm_settings.echo,
                 mdm_settings.verbose);
        break;
    case 2:
        snprintf(buf, buf_size, "S0:%03u S1:%03u S2:%03u S3:%03u S4:%03u S5:%03u \r\n\r\n",
                 mdm_settings.auto_answer,
                 0, // TODO ring counter
                 mdm_settings.escChar,
                 mdm_settings.crChar,
                 mdm_settings.lfChar,
                 mdm_settings.bsChar);
        break;
    case 3:
        snprintf(buf, buf_size, "STORED PROFILE:\r\n");
        break;
    case 4:
        snprintf(buf, buf_size, "E%u V%u\r\n",
                 nvr_settings.echo,
                 nvr_settings.verbose);
        break;
    case 5:
        snprintf(buf, buf_size, "S0:%03u S2:%03u S3:%03u S4:%03u S5:%03u \r\n\r\n",
                 nvr_settings.auto_answer,
                 nvr_settings.escChar,
                 nvr_settings.crChar,
                 nvr_settings.lfChar,
                 nvr_settings.bsChar);
        __attribute__((fallthrough));
    default:
        return -1;
    }
    return ++state;
}

// &V
static bool cmd_view_config(const char **s)
{
    switch (cmd_parse_num(s))
    {
    case -1:
        mdm_set_response_fn(cmd_view_config_response, 0);
        return true;
    }
    return false;
}

// &
static bool cmd_parse_amp(const char **s)
{
    char ch = **s;
    ++*s;
    switch (toupper(ch))
    {
    case 'F':
        return cmd_load_factory(s);
    case 'V':
        return cmd_view_config(s);
    }
    --*s;
    return false;
}

// Parse AT command (without the AT)
bool cmd_parse(const char **s)
{
    char ch = **s;
    ++*s;
    switch (toupper(ch))
    {
    case 'E':
        return cmd_echo(s);
    case 'V':
        return cmd_verbose(s);
    case '&':
        return cmd_parse_amp(s);
    }
    --*s;
    return false;
}
