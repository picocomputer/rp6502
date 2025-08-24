/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "net/cmd.h"
#include "net/mdm.h"
#include "sys/cfg.h"
#include <stdio.h>
#include <ctype.h>
#include <string.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_CMD)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// The design philosophy here is to use AT+XXX? and AT+XXX=YYY
// for everything modern like WiFi and telnet configuration.
// The traditional commands are then free to act like an actual
// Hayes-like modem.

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

// D
static bool cmd_dial(const char **s)
{
    const char *address;
    if (toupper((*s)[0]) == 'S' && (*s)[1] == '=')
    {
        *s += 2;
        int num = cmd_parse_num(s);
        if (num < 0)
            num = 0;
        if (num > MDM_PHONEBOOK_ENTRIES || (*s)[0])
            return false;
        address = mdm_read_phonebook_entry(num);
    }
    else
    {
        address = *s;
        *s += strlen(*s);
    }
    return mdm_dial(address);
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

// H, H0
static bool cmd_hook(const char **s)
{
    switch (cmd_parse_num(s))
    {
    case -1:
    case 0:
        return mdm_hangup();
    }
    return false;
}

// O, O0
static bool cmd_online(const char **s)
{
    switch (cmd_parse_num(s))
    {
    case -1:
    case 0:
        return mdm_connect();
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

// X0, X1
static bool cmd_progress(const char **s)
{
    int value = cmd_parse_num(s);
    if (value >= 0 && value <= 1)
    {
        mdm_settings.progress = value;
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
    mdm_settings_t nvr_settings;
    switch (state)
    {
    case 0:
        snprintf(buf, buf_size, "ACTIVE PROFILE:\r\n");
        break;
    case 1:
        snprintf(buf, buf_size, "E%u Q%u V%u X%u\r\n",
                 mdm_settings.echo,
                 mdm_settings.quiet,
                 mdm_settings.verbose,
                 mdm_settings.progress);
        break;
    case 2:
        snprintf(buf, buf_size, "S0:%03u S1:%03u S2:%03u S3:%03u S4:%03u S5:%03u\r\n",
                 mdm_settings.auto_answer,
                 0, // TODO ring counter
                 mdm_settings.esc_char,
                 mdm_settings.cr_char,
                 mdm_settings.lf_char,
                 mdm_settings.bs_char);
        break;
    case 3:
        snprintf(buf, buf_size, "\r\nSTORED PROFILE:\r\n");
        break;
    case 4:
        mdm_read_settings(&nvr_settings);
        snprintf(buf, buf_size, "E%u Q%u V%u X%u\r\n",
                 nvr_settings.echo,
                 nvr_settings.quiet,
                 nvr_settings.verbose,
                 nvr_settings.progress);
        break;
    case 5:
        mdm_read_settings(&nvr_settings);
        snprintf(buf, buf_size, "S0:%03u S2:%03u S3:%03u S4:%03u S5:%03u\r\n",
                 nvr_settings.auto_answer,
                 nvr_settings.esc_char,
                 nvr_settings.cr_char,
                 nvr_settings.lf_char,
                 nvr_settings.bs_char);
        break;
    case 6:
        snprintf(buf, buf_size, "\r\nTELEPHONE NUMBERS:\r\n");
        break;
    case 7:
        snprintf(buf, buf_size, "0=%s\r\n", mdm_read_phonebook_entry(0));
        break;
    case 8:
        snprintf(buf, buf_size, "1=%s\r\n", mdm_read_phonebook_entry(1));
        break;
    case 9:
        snprintf(buf, buf_size, "2=%s\r\n", mdm_read_phonebook_entry(2));
        break;
    case 10:
        snprintf(buf, buf_size, "3=%s\r\n", mdm_read_phonebook_entry(3));
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

// &Z
static bool cmd_save_phonebook(const char **s)
{
    int num = 0;
    // supports hayes-ism AT&Z5551212
    if ((*s)[0] == 0 || (*s)[0] == '=' || (*s)[1] == '=')
    {
        num = cmd_parse_num(s);
        if (num == -1)
            num = 0;
        if (num >= MDM_PHONEBOOK_ENTRIES)
            return false;
        if ((*s)[0] != '=')
            return false;
        ++*s;
    }
    bool result = mdm_write_phonebook_entry(*s, num);
    *s += strlen(*s);
    return result;
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
    case 'Z':
        return cmd_save_phonebook(s);
    }
    --*s;
    return false;
}

// +RF?
static int cmd_plus_rf_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, "%u\r\n", cfg_get_rf());
    return -1;
}

// +RF
static bool cmd_plus_rf(const char **s)
{
    char ch = **s;
    ++*s;
    switch (toupper(ch))
    {
    case '=':
        return cfg_set_rf(cmd_parse_num(s));
    case '?':
        mdm_set_response_fn(cmd_plus_rf_response, 0);
        return true;
    }
    --*s;
    return false;
}

// +RFCC?
static int cmd_plus_rfcc_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    const char *cc = cfg_get_rfcc();
    snprintf(buf, buf_size, "%s\r\n", strlen(cc) ? cc : "Worldwide");
    return -1;
}

// +RFCC
static bool cmd_plus_rfcc(const char **s)
{
    char ch = **s;
    ++*s;
    switch (toupper(ch))
    {
    case '=':
        bool result = cfg_set_rfcc(*s);
        *s += strlen(*s);
        return result;
    case '?':
        mdm_set_response_fn(cmd_plus_rfcc_response, 0);
        return true;
    }
    --*s;
    return false;
}

// +SSID?
static int cmd_plus_ssid_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, "%s\r\n", cfg_get_ssid());
    return -1;
}

// +SSID
static bool cmd_plus_ssid(const char **s)
{
    char ch = **s;
    ++*s;
    switch (toupper(ch))
    {
    case '=':
        bool result = cfg_set_ssid(*s);
        *s += strlen(*s);
        return result;
    case '?':
        mdm_set_response_fn(cmd_plus_ssid_response, 0);
        return true;
    }
    --*s;
    return false;
}

// +PASS?
static int cmd_plus_pass_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, "%s\r\n", strlen(cfg_get_pass()) ? "(set)" : "(none)");
    return -1;
}

// +PASS
static bool cmd_plus_pass(const char **s)
{
    char ch = **s;
    ++*s;
    switch (toupper(ch))
    {
    case '=':
        bool result = cfg_set_pass(*s);
        *s += strlen(*s);
        return result;
    case '?':
        mdm_set_response_fn(cmd_plus_pass_response, 0);
        return true;
    }
    --*s;
    return false;
}

// +
static bool cmd_parse_modern(const char **s)
{
    if (!strncasecmp(*s, "RFCC", 4))
    {
        (*s) += 4;
        return cmd_plus_rfcc(s);
    }
    if (!strncasecmp(*s, "RF", 2))
    {
        (*s) += 2;
        return cmd_plus_rf(s);
    }
    if (!strncasecmp(*s, "SSID", 4))
    {
        (*s) += 4;
        return cmd_plus_ssid(s);
    }
    if (!strncasecmp(*s, "PASS", 4))
    {
        (*s) += 4;
        return cmd_plus_pass(s);
    }
    return false;
}

// Parse AT command (without the AT)
bool cmd_parse(const char **s)
{
    char ch = **s;
    ++*s;
    switch (toupper(ch))
    {
    case 'D':
        return cmd_dial(s);
    case 'E':
        return cmd_echo(s);
    case 'F':
        return cmd_online_echo(s);
    case 'H':
        return cmd_hook(s);
    case 'O':
        return cmd_online(s);
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
    case 'X':
        return cmd_progress(s);
    case 'Z':
        return cmd_reset(s);
    case '&':
        return cmd_parse_amp(s);
    case '+':
        return cmd_parse_modern(s);
    }
    --*s;
    return false;
}
