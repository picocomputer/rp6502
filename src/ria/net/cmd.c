/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "net/cmd.h"
#include "net/cyw.h"
#include "net/mdm.h"
#include "net/wfi.h"
#include "str/str.h"
#include <stdio.h>
#include <ctype.h>
#include <string.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_CMD)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
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
        // Hayes: bare ATDS= means entry 0.
        if (num < 0)
            num = 0;
        if (num >= MDM_PHONEBOOK_ENTRIES || (*s)[0])
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
        mdm_settings->echo = 0;
        return true;
    case 1:
        mdm_settings->echo = 1;
        return true;
    }
    return false;
}

// F1
static bool cmd_duplex(const char **s)
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
static bool cmd_hangup(const char **s)
{
    switch (cmd_parse_num(s))
    {
    case -1:
    case 0:
        mdm_hangup();
        return true;
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
        mdm_settings->quiet = 0;
        return true;
    case 1:
        mdm_settings->quiet = 1;
        return true;
    case 2:
        mdm_settings->quiet = 2;
        return true;
    }
    return false;
}

static int cmd_s_query_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    uint8_t val = 0;
    switch (mdm_settings->s_pointer)
    {
    case 0:
        val = mdm_settings->auto_answer;
        break;
    case 1:
        val = mdm_get_ring_count();
        break;
    case 2:
        val = mdm_settings->esc_char;
        break;
    case 3:
        val = mdm_settings->cr_char;
        break;
    case 4:
        val = mdm_settings->lf_char;
        break;
    case 5:
        val = mdm_settings->bs_char;
        break;
    }
    snprintf(buf, buf_size, "%u\r\n", val);
    return -1;
}

// Sxxx
static bool cmd_s_pointer(const char **s)
{
    int num = cmd_parse_num(s);
    // Hayes: bare ATS selects S0.
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
        mdm_settings->s_pointer = num;
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
    // Hayes: bare ATS= writes 0.
    if (num < 0)
        num = 0;
    switch (mdm_settings->s_pointer)
    {
    case 0:
        mdm_settings->auto_answer = num;
        return true;
    case 2:
        mdm_settings->esc_char = num;
        return true;
    case 3:
        mdm_settings->cr_char = num;
        return true;
    case 4:
        mdm_settings->lf_char = num;
        return true;
    case 5:
        mdm_settings->bs_char = num;
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
        mdm_settings->verbose = 0;
        return true;
    case 1:
        mdm_settings->verbose = 1;
        return true;
    }
    return false;
}

// X0, X1
static bool cmd_progress(const char **s)
{
    int value = cmd_parse_num(s);
    if (value >= 0 && value <= 4)
    {
        mdm_settings->progress = value;
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
        return mdm_read_settings(mdm_settings);
    }
    return false;
}

// &F
static bool cmd_load_factory(const char **s)
{
    switch (cmd_parse_num(s))
    {
    case -1:
    case 0:
        mdm_factory_settings(mdm_settings);
        return true;
    }
    return false;
}

// &V
static int cmd_view_config_response(char *buf, size_t buf_size, int state)
{
    mdm_settings_t stored_settings;
    if (!mdm_settings_persistent() && state >= 6 && state <= 10)
        state = 11;
    switch (state)
    {
    case 0:
        snprintf(buf, buf_size, "ACTIVE PROFILE:\r\n");
        break;
    case 1:
        snprintf(buf, buf_size, "E%u Q%u V%u X%u \\L%u \\N%u \\T=%s\r\n",
                 mdm_settings->echo,
                 mdm_settings->quiet,
                 mdm_settings->verbose,
                 mdm_settings->progress,
                 mdm_settings->listen_port,
                 mdm_settings->net_mode,
                 mdm_settings->tty_type);
        break;
    case 2:
        snprintf(buf, buf_size, "S0:%03u S1:%03u S2:%03u S3:%03u S4:%03u S5:%03u\r\n",
                 mdm_settings->auto_answer,
                 mdm_get_ring_count(),
                 mdm_settings->esc_char,
                 mdm_settings->cr_char,
                 mdm_settings->lf_char,
                 mdm_settings->bs_char);
        break;
    case 3:
        snprintf(buf, buf_size, "\r\nSTORED PROFILE:\r\n");
        break;
    case 4:
        mdm_read_settings(&stored_settings);
        snprintf(buf, buf_size, "E%u Q%u V%u X%u \\L%u \\N%u \\T=%s\r\n",
                 stored_settings.echo,
                 stored_settings.quiet,
                 stored_settings.verbose,
                 stored_settings.progress,
                 stored_settings.listen_port,
                 stored_settings.net_mode,
                 stored_settings.tty_type);
        break;
    case 5:
        mdm_read_settings(&stored_settings);
        snprintf(buf, buf_size, "S0:%03u S2:%03u S3:%03u S4:%03u S5:%03u\r\n",
                 stored_settings.auto_answer,
                 stored_settings.esc_char,
                 stored_settings.cr_char,
                 stored_settings.lf_char,
                 stored_settings.bs_char);
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
        break;
    case 11:
        snprintf(buf, buf_size, "\r\nNETWORK:\r\n");
        break;
    case 12:
        snprintf(buf, buf_size, "+RF=%u\r\n", cyw_get_rf_enable());
        break;
    case 13:
    {
        const char *cc = cyw_get_rf_country_code();
        snprintf(buf, buf_size, "+RFCC=%s\r\n", strlen(cc) ? cc : STR_WORLDWIDE);
        break;
    }
    case 14:
        snprintf(buf, buf_size, "+SSID=%s\r\n", wfi_get_ssid());
        break;
    case 15:
        snprintf(buf, buf_size, "+PASS=%s\r\n",
                 strlen(wfi_get_pass()) ? STR_PARENS_SET : STR_PARENS_NONE);
        break;
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
        return mdm_write_settings(mdm_settings);
    }
    return false;
}

// &Z
static bool cmd_save_phonebook(const char **s)
{
    if ((*s)[0] == 0)
        return false;
    unsigned num = 0;
    // Look for a digits-only prefix terminated by '=' (the slot+eq form).
    // Fall through to the hayes-ism AT&Z5551212 (bare number into slot 0)
    // when no such prefix is present.
    const char *p = *s;
    while (*p >= '0' && *p <= '9')
        p++;
    if (*p == '=' && p > *s)
    {
        int parsed = cmd_parse_num(s);
        if (parsed >= MDM_PHONEBOOK_ENTRIES)
            return false;
        num = (unsigned)parsed;
        ++*s;
    }
    else if ((*s)[0] == '=')
    {
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
    snprintf(buf, buf_size, "%u\r\n", cyw_get_rf_enable());
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
        return cyw_set_rf_enable(cmd_parse_num(s));
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
    const char *cc = cyw_get_rf_country_code();
    snprintf(buf, buf_size, "%s\r\n", strlen(cc) ? cc : STR_WORLDWIDE);
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
    {
        bool result = cyw_set_rf_country_code(*s);
        *s += strlen(*s);
        return result;
    }
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
    snprintf(buf, buf_size, "%s\r\n", wfi_get_ssid());
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
    {
        bool result = wfi_set_ssid(*s);
        *s += strlen(*s);
        return result;
    }
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
    snprintf(buf, buf_size, "%s\r\n", strlen(wfi_get_pass()) ? STR_PARENS_SET : STR_PARENS_NONE);
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
    {
        bool result = wfi_set_pass(*s);
        *s += strlen(*s);
        return result;
    }
    case '?':
        mdm_set_response_fn(cmd_plus_pass_response, 0);
        return true;
    }
    --*s;
    return false;
}

// +
static bool cmd_parse_plus(const char **s)
{
    if (!strncasecmp(*s, STR_RFCC, STR_RFCC_LEN))
    {
        *s += STR_RFCC_LEN;
        return cmd_plus_rfcc(s);
    }
    if (!strncasecmp(*s, STR_RF, STR_RF_LEN))
    {
        *s += STR_RF_LEN;
        return cmd_plus_rf(s);
    }
    if (!strncasecmp(*s, STR_SSID, STR_SSID_LEN))
    {
        *s += STR_SSID_LEN;
        return cmd_plus_ssid(s);
    }
    if (!strncasecmp(*s, STR_PASS, STR_PASS_LEN))
    {
        *s += STR_PASS_LEN;
        return cmd_plus_pass(s);
    }
    return false;
}

// \N?
static int cmd_backslash_n_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, "%u\r\n", mdm_settings->net_mode);
    return -1;
}

// \N
static bool cmd_backslash_n(const char **s)
{
    char ch = **s;
    if (ch == '?')
    {
        ++*s;
        mdm_set_response_fn(cmd_backslash_n_response, 0);
        return true;
    }
    int num = cmd_parse_num(s);
    if (num >= 0 && num <= 1)
    {
        mdm_settings->net_mode = num;
        return true;
    }
    return false;
}

// \T?
static int cmd_backslash_t_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, "%s\r\n", mdm_settings->tty_type);
    return -1;
}

// \T
static bool cmd_backslash_t(const char **s)
{
    char ch = **s;
    ++*s;
    switch (ch)
    {
    case '?':
        mdm_set_response_fn(cmd_backslash_t_response, 0);
        return true;
    case '=':
    {
        size_t len = strlen(*s);
        if (len >= sizeof(mdm_settings->tty_type))
            return false;
        strcpy(mdm_settings->tty_type, *s);
        *s += len;
        return true;
    }
    }
    --*s;
    return false;
}

// \L?
static int cmd_backslash_l_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, "%u\r\n", mdm_settings->listen_port);
    return -1;
}

// \L
static bool cmd_backslash_l(const char **s)
{
    char ch = **s;
    if (ch == '?')
    {
        ++*s;
        mdm_set_response_fn(cmd_backslash_l_response, 0);
        return true;
    }
    int num = cmd_parse_num(s);
    if (num >= 0 && num <= 65535)
        return mdm_set_listen_port((uint16_t)num);
    return false;
}

// backslash
static bool cmd_parse_backslash(const char **s)
{
    char ch = **s;
    ++*s;
    switch (toupper(ch))
    {
    case 'L':
        return cmd_backslash_l(s);
    case 'N':
        return cmd_backslash_n(s);
    case 'T':
        return cmd_backslash_t(s);
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
    case 'A':
        return mdm_answer();
    case 'D':
        return cmd_dial(s);
    case 'E':
        return cmd_echo(s);
    case 'F':
        return cmd_duplex(s);
    case 'H':
        return cmd_hangup(s);
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
        return cmd_parse_plus(s);
    case '\\':
        return cmd_parse_backslash(s);
    }
    --*s;
    return false;
}
