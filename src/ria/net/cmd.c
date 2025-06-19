/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "net/cmd.h"
#include "net/mdm.h"
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

// F1
static bool cmd_online_echo(const char **s)
{
    // F0 support was dropped in Hayes V.series.
    // F1 succeeds for compatibility.
    switch (cmd_parse_num(s))
    {
    case 1:
        return true;
    }
    return false;
}

// Q0, Q1, Q2
static bool cmd_quiet(const char **s)
{
    switch (cmd_parse_num(s))
    {
    case 0:
        mdm_settings.quiet = 0;
        return true;
    case 1:
        mdm_settings.quiet = 1;
        return true;
    case 2:
        mdm_settings.quiet = 2;
        return true;
    }
    return false;
}

static int cmd_s_query_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    uint8_t val = 0;
    switch (mdm_settings.s_pointer)
    {
    case 0:
        val = mdm_settings.auto_answer;
        break;
    case 1:
        val = 0; // TODO ring count
        break;
    case 2:
        val = mdm_settings.esc_char;
        break;
    case 3:
        val = mdm_settings.cr_char;
        break;
    case 4:
        val = mdm_settings.lf_char;
        break;
    case 5:
        val = mdm_settings.bs_char;
        break;
    }
    snprintf(buf, buf_size, "%u\r\n", val);
    return -1;
}

// Sxxx
static bool cmd_s_pointer(const char **s)
{
    int num = cmd_parse_num(s);
    if (num < 0)
        num = 0;
    switch (num)
    {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
        mdm_settings.s_pointer = num;
        return true;
    default:
        return false;
    }
}

// S?
static bool cmd_s_query(const char **s)
{
    (void)s;
    mdm_set_response_fn(cmd_s_query_response, 0);
    return true;
}

// S=
static bool cmd_s_set(const char **s)
{
    int num = cmd_parse_num(s);
    if (num < 0)
        num = 0;
    switch (mdm_settings.s_pointer)
    {
    case 0:
        mdm_settings.auto_answer = num;
        return true;
    case 2:
        mdm_settings.esc_char = num;
        return true;
    case 3:
        mdm_settings.cr_char = num;
        return true;
    case 4:
        mdm_settings.lf_char = num;
        return true;
    case 5:
        mdm_settings.bs_char = num;
        return true;
    default:
        return false;
    }
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

// Z, Z0
static bool cmd_reset(const char **s)
{
    switch (cmd_parse_num(s))
    {
    case -1:
    case 0:
        return mdm_read_settings(&mdm_settings);
    }
    return false;
}

// &F
static bool cmd_load_factory(const char **s)
{
    switch (cmd_parse_num(s))
    {
    case -1:
        mdm_factory_settings(&mdm_settings);
        return true;
    }
    return false;
}

// &V
static int cmd_view_config_response(char *buf, size_t buf_size, int state)
{
    mdm_settings_t mdm_settings;
    if (state >= 4)
        mdm_read_settings(&mdm_settings);

    switch (state)
    {
    case 0:
        snprintf(buf, buf_size, "ACTIVE PROFILE:\r\n");
        break;
    case 1:
        snprintf(buf, buf_size, "E%u Q%u V%u\r\n",
                 mdm_settings.echo,
                 mdm_settings.quiet,
                 mdm_settings.verbose);
        break;
    case 2:
        snprintf(buf, buf_size, "S0:%03u S1:%03u S2:%03u S3:%03u S4:%03u S5:%03u\r\n\r\n",
                 mdm_settings.auto_answer,
                 0, // TODO ring counter
                 mdm_settings.esc_char,
                 mdm_settings.cr_char,
                 mdm_settings.lf_char,
                 mdm_settings.bs_char);
        break;
    case 3:
        snprintf(buf, buf_size, "STORED PROFILE:\r\n");
        break;
    case 4:
        snprintf(buf, buf_size, "E%u Q%u V%u\r\n",
                 mdm_settings.echo,
                 mdm_settings.quiet,
                 mdm_settings.verbose);
        break;
    case 5:
        snprintf(buf, buf_size, "S0:%03u S2:%03u S3:%03u S4:%03u S5:%03u\r\n\r\n",
                 mdm_settings.auto_answer,
                 mdm_settings.esc_char,
                 mdm_settings.cr_char,
                 mdm_settings.lf_char,
                 mdm_settings.bs_char);
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

// &W, &W0
static bool cmd_save_nvram(const char **s)
{
    switch (cmd_parse_num(s))
    {
    case -1:
    case 0:
        return mdm_write_settings(&mdm_settings);
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
    case 'W':
        return cmd_save_nvram(s);
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
    case 'F':
        return cmd_online_echo(s);
    case 'Q':
        return cmd_quiet(s);
    case 'S':
        return cmd_s_pointer(s);
    case '?':
        return cmd_s_query(s);
    case '=':
        return cmd_s_set(s);
    case 'V':
        return cmd_verbose(s);
    case 'Z':
        return cmd_reset(s);
    case '&':
        return cmd_parse_amp(s);
    }
    --*s;
    return false;
}
