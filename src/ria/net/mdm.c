/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W
#include "net/mdm.h"
void mdm_task(void) {}
void mdm_stop(void) {}
void mdm_init(void) {}
bool mdm_open(const char *) { return false; }
bool mdm_close(void) { return false; }
int mdm_rx(char *) { return -1; }
int mdm_tx(char) { return -1; }
#else

#include "net/cmd.h"
#include "net/mdm.h"
#include "net/tel.h"
#include "sys/lfs.h"
#include "sys/mem.h"
#include <pico/time.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_MDM)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Leave a little room for escaped telnet characters.
#if TCP_MSS == 536 // does not fragment and good enough
#define MDM_TX_BUF_SIZE (512)
#elif TCP_MSS == 1460 // in case someone wants to try
#define MDM_TX_BUF_SIZE (1024)
#else
#error unexpected TCP_MSS
#endif
static char mdm_tx_buf[MDM_TX_BUF_SIZE];
static size_t mdm_tx_buf_len;

#define MDM_ESCAPE_GUARD_TIME_US 1000000
#define MDM_ESCAPE_COUNT 3

// Old modems have 40 chars, Hayes V.series has 255.
#define MDM_AT_COMMAND_LEN (255)
static char mdm_cmd_buf[MDM_AT_COMMAND_LEN + 1];
static size_t mdm_cmd_buf_len;

// Must fit 80 columns plus a couple CRLFs
#define MDM_RESPONSE_BUF_SIZE (128)
static char mdm_response_buf[MDM_RESPONSE_BUF_SIZE];
static size_t mdm_response_buf_head;
static size_t mdm_response_buf_tail;
static int (*mdm_response_fn)(char *, size_t, int);
static int mdm_response_state;

typedef enum
{
    mdm_state_on_hook,
    mdm_state_dialing,
    mdm_state_connected,
} mdm_state_t;
static mdm_state_t mdm_state;
static bool mdm_in_command_mode;
static bool mdm_is_parsing;
static const char *mdm_parse_str;
static bool mdm_parse_result;
static bool mdm_is_open;
static unsigned mdm_escape_count;
static absolute_time_t mdm_escape_last_char;
static absolute_time_t mdm_escape_guard;

mdm_settings_t mdm_settings;

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

static const char __in_flash("net_mdm") phone0_sys[] = "PHONE0.SYS";
static const char __in_flash("net_mdm") phone0_tmp[] = "PHONE0.TMP";
static const char __in_flash("net_mdm") modem0_sys[] = "MODEM0.SYS";
static const char __in_flash("net_mdm") devicename[] = "AT:";
static const char __in_flash("net_mdm") devicename0[] = "AT0:";

void mdm_stop(void)
{
    tel_close();
    mdm_is_open = false;
    mdm_cmd_buf_len = 0;
    mdm_tx_buf_len = 0;
    mdm_response_buf_head = 0;
    mdm_response_buf_tail = 0;
    mdm_response_state = -1;
    mdm_parse_result = true;
    mdm_state = mdm_state_on_hook;
    mdm_in_command_mode = true;
    mdm_is_parsing = false;
    mdm_escape_count = 0;
}

void mdm_init(void)
{
    mdm_stop();
}

bool mdm_open(const char *filename)
{
    if (mdm_is_open)
        return false;
    if (!strncasecmp(filename, devicename, 3))
        filename += strlen(devicename0);
    else if (!strncasecmp(filename, devicename0, 4))
        filename += strlen(devicename0);
    else
        return false;
    mdm_read_settings(&mdm_settings);
    mdm_is_open = true;
    // Optionally process filename as AT command
    // after NVRAM read. e.g. AT:&F
    if (filename[0])
    {
        mdm_is_parsing = true;
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

static inline bool mdm_response_buf_empty(void)
{
    return mdm_response_buf_head == mdm_response_buf_tail;
}

static inline bool mdm_response_buf_full(void)
{
    return ((mdm_response_buf_head + 1) % MDM_RESPONSE_BUF_SIZE) == mdm_response_buf_tail;
}

static inline size_t mdm_response_buf_count(void)
{
    if (mdm_response_buf_head >= mdm_response_buf_tail)
        return mdm_response_buf_head - mdm_response_buf_tail;
    else
        return MDM_RESPONSE_BUF_SIZE - mdm_response_buf_tail + mdm_response_buf_head;
}

void mdm_set_response_fn(int (*fn)(char *, size_t, int), int state)
{
    if (mdm_response_state >= 0)
    {
#ifndef NDEBUG
        assert(false);
#endif
        // Responses aren't being consumed.
        // This shouldn't happen, but what the
        // 6502 app does is beyond our control.
        // Discard all the old data. This way the
        // 6502 app doesn't get a mix of old and new
        // data when it finally decides to wake up.
        mdm_response_buf_head = mdm_response_buf_tail = 0;
    }
    mdm_response_fn = fn;
    mdm_response_state = state;
}

static void mdm_response_append(char ch)
{
    if (!mdm_response_buf_full())
    {
        mdm_response_buf[mdm_response_buf_head] = ch;
        mdm_response_buf_head = (mdm_response_buf_head + 1) % MDM_RESPONSE_BUF_SIZE;
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
    // Get next line, if needed and in progress
    if (mdm_response_buf_empty() && mdm_response_state >= 0)
    {
        mdm_response_state = mdm_response_fn(mdm_response_buf, MDM_RESPONSE_BUF_SIZE, mdm_response_state);
        mdm_response_buf_head = strlen(mdm_response_buf);
        mdm_response_buf_tail = 0;
        // Translate CR and LF chars to settings
        for (size_t i = 0; i < mdm_response_buf_head; i++)
        {
            uint8_t swap_ch = 0;
            if (mdm_response_buf[i] == '\r')
                swap_ch = mdm_response_buf[i] = mdm_settings.cr_char;
            if (mdm_response_buf[i] == '\n')
                swap_ch = mdm_response_buf[i] = mdm_settings.lf_char;
            if (swap_ch & 0x80)
            {
                for (size_t j = i; j < mdm_response_buf_head; j++)
                    mdm_response_buf[j] = mdm_response_buf[j + 1];
                mdm_response_buf_head--;
            }
        }
    }
    // Get from line buffer, if available
    if (!mdm_response_buf_empty())
    {
        *ch = mdm_response_buf[mdm_response_buf_tail];
        mdm_response_buf_tail = (mdm_response_buf_tail + 1) % MDM_RESPONSE_BUF_SIZE;
        return 1;
    }
    // Get from telephone emulator
    if (!mdm_in_command_mode)
        return tel_rx(ch);
    return 0;
}

static bool mdm_cmd_buf_is_at_command(void)
{
    return (mdm_cmd_buf[0] == 'a' || mdm_cmd_buf[0] == 'A') &&
           (mdm_cmd_buf[1] == 't' || mdm_cmd_buf[1] == 'T');
}

static int mdm_tx_command_mode(char ch)
{
    if (mdm_response_state >= 0)
        return 0;
    if (ch == '\r' || (!(mdm_settings.cr_char & 0x80) && ch == mdm_settings.cr_char))
    {
        if (mdm_settings.echo)
            mdm_response_append_cr_lf();
        mdm_cmd_buf[mdm_cmd_buf_len] = 0;
        mdm_cmd_buf_len = 0;
        if (mdm_cmd_buf_is_at_command())
        {
            if (!mdm_settings.echo && !mdm_settings.quiet && mdm_settings.verbose)
                mdm_response_append_cr_lf();
            mdm_is_parsing = true;
            mdm_parse_result = true;
            mdm_parse_str = &mdm_cmd_buf[2];
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
        if (mdm_cmd_buf_len)
            mdm_cmd_buf[--mdm_cmd_buf_len] = 0;
    }
    else if (ch >= 32 && ch < 127)
    {
        if (mdm_settings.echo)
            mdm_response_append(ch);
        if (ch == '/' && mdm_cmd_buf_len == 1)
        {
            if (mdm_settings.echo || (!mdm_settings.quiet && mdm_settings.verbose))
                mdm_response_append_cr_lf();
            mdm_cmd_buf_len = 0;
            mdm_is_parsing = true;
            if (mdm_cmd_buf_is_at_command())
            {

                mdm_parse_result = true;
                mdm_parse_str = &mdm_cmd_buf[2];
            }
            else
                mdm_parse_result = false; // immediate error
            return 1;
        }
        if (mdm_cmd_buf_len < MDM_AT_COMMAND_LEN)
            mdm_cmd_buf[mdm_cmd_buf_len++] = ch;
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

static void mdm_tx_escape_observer(char ch)
{
    bool last_char_guarded = absolute_time_diff_us(mdm_escape_last_char, get_absolute_time()) >
                             MDM_ESCAPE_GUARD_TIME_US;
    if (mdm_escape_count && last_char_guarded)
        mdm_escape_count = 0;
    if (mdm_settings.esc_char < 128 && (mdm_escape_count || last_char_guarded))
    {
        if (ch != mdm_settings.esc_char)
            mdm_escape_count = 0;
        else if (++mdm_escape_count == MDM_ESCAPE_COUNT)
            mdm_escape_guard = make_timeout_time_us(MDM_ESCAPE_GUARD_TIME_US);
    }
    mdm_escape_last_char = get_absolute_time();
}

int mdm_tx(char ch)
{
    if (!mdm_is_open)
        return -1;
    mdm_tx_escape_observer(ch);
    if (mdm_in_command_mode)
    {
        if (!mdm_is_parsing)
            return mdm_tx_command_mode(ch);
    }
    else if (mdm_state == mdm_state_connected)
        return mdm_tx_connected(ch);
    else if (mdm_state == mdm_state_dialing)
        return 1;
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

void mdm_factory_settings(mdm_settings_t *settings)
{
    settings->s_pointer = 0;   // S0 (not saved)
    settings->echo = 1;        // E1
    settings->quiet = 0;       // Q0
    settings->verbose = 1;     // V1
    settings->progress = 0;    // X0
    settings->auto_answer = 0; // S0=0
    settings->esc_char = '+';  // S2=43
    settings->cr_char = '\r';  // S3=13
    settings->lf_char = '\n';  // S4=10
    settings->bs_char = '\b';  // S5=8
}

const char *mdm_read_phonebook_entry(unsigned index)
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, phone0_sys,
                                     LFS_O_RDONLY, &lfs_file_config);
    mbuf[0] = 0;
    if (lfsresult < 0)
        return (char *)mbuf;
    for (; lfs_gets((char *)mbuf, MBUF_SIZE, &lfs_volume, &lfs_file); index--)
    {
        size_t len = strlen((char *)mbuf);
        while (len && mbuf[len - 1] == '\n')
            len--;
        mbuf[len] = 0;
        if (index == 0)
            break;
    }
    lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfsresult < 0)
        DBG("?Unable to lfs_file_close %s (%d)\n", phone0_sys, lfsresult);
    if (index)
        mbuf[0] = 0;
    return (char *)mbuf;
}

bool mdm_write_phonebook_entry(const char *entry, unsigned index)
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, phone0_tmp,
                                     LFS_O_RDWR | LFS_O_CREAT,
                                     &lfs_file_config);
    if (lfsresult < 0)
    {
        DBG("?Unable to lfs_file_opencfg %s for writing (%d)\n", phone0_tmp, lfsresult);
        return false;
    }
    for (unsigned i = 0; i < MDM_PHONEBOOK_ENTRIES; i++)
    {
        if (i == index)
            lfsresult = lfs_printf(&lfs_volume, &lfs_file, "%s\n", entry);
        else
            lfsresult = lfs_printf(&lfs_volume, &lfs_file, "%s\n", mdm_read_phonebook_entry(i));
        if (lfsresult < 0)
            DBG("?Unable to write %s contents (%d)\n", phone0_tmp, lfsresult);
    }
    int lfscloseresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfscloseresult < 0)
        DBG("?Unable to lfs_file_close %s (%d)\n", phone0_tmp, lfscloseresult);
    if (lfsresult < 0 || lfscloseresult < 0)
    {
        lfs_remove(&lfs_volume, phone0_tmp);
        return false;
    }
    lfsresult = lfs_remove(&lfs_volume, phone0_sys);
    if (lfsresult < 0 && lfsresult != LFS_ERR_NOENT)
    {
        DBG("?Unable to lfs_remove %s (%d)\n", phone0_sys, lfsresult);
        return false;
    }
    lfsresult = lfs_rename(&lfs_volume, phone0_tmp, phone0_sys);
    if (lfsresult < 0)
    {
        DBG("?Unable to lfs_rename (%d)\n", lfsresult);
        return false;
    }
    return true;
}

bool mdm_write_settings(const mdm_settings_t *settings)
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, modem0_sys,
                                     LFS_O_RDWR | LFS_O_CREAT,
                                     &lfs_file_config);
    if (lfsresult < 0)
        DBG("?Unable to lfs_file_opencfg %s for writing (%d)\n", modem0_sys, lfsresult);
    if (lfsresult >= 0)
        if ((lfsresult = lfs_file_truncate(&lfs_volume, &lfs_file, 0)) < 0)
            DBG("?Unable to lfs_file_truncate %s (%d)\n", modem0_sys, lfsresult);
    if (lfsresult >= 0)
    {
        lfsresult = lfs_printf(&lfs_volume, &lfs_file,
                               "E%u\n"
                               "Q%u\n"
                               "V%u\n"
                               "X%u\n"
                               "S0=%u\n"
                               "S2=%u\n"
                               "S3=%u\n"
                               "S4=%u\n"
                               "S5=%u\n"
                               "",
                               settings->echo,
                               settings->quiet,
                               settings->verbose,
                               settings->progress,
                               settings->auto_answer,
                               settings->esc_char,
                               settings->cr_char,
                               settings->lf_char,
                               settings->bs_char);
        if (lfsresult < 0)
            DBG("?Unable to write %s contents (%d)\n", modem0_sys, lfsresult);
    }
    int lfscloseresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfscloseresult < 0)
        DBG("?Unable to lfs_file_close %s (%d)\n", modem0_sys, lfscloseresult);
    if (lfsresult < 0 || lfscloseresult < 0)
    {
        lfs_remove(&lfs_volume, modem0_sys);
        return false;
    }
    return true;
}

bool mdm_read_settings(mdm_settings_t *settings)
{
    mdm_factory_settings(settings);
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, modem0_sys,
                                     LFS_O_RDONLY, &lfs_file_config);
    mbuf[0] = 0;
    if (lfsresult < 0)
    {
        if (lfsresult == LFS_ERR_NOENT)
            return true;
        DBG("?Unable to lfs_file_opencfg %s for reading (%d)\n", modem0_sys, lfsresult);
        return false;
    }
    while (lfs_gets((char *)mbuf, MBUF_SIZE, &lfs_volume, &lfs_file))
    {
        size_t len = strlen((char *)mbuf);
        while (len && mbuf[len - 1] == '\n')
            len--;
        mbuf[len] = 0;
        const char *str = (char *)(mbuf + 1);
        len -= 1;
        switch (mbuf[0])
        {
        case 'E':
            settings->echo = atoi(str);
            break;
        case 'Q':
            settings->quiet = atoi(str);
            break;
        case 'V':
            settings->verbose = atoi(str);
            break;
        case 'X':
            settings->progress = atoi(str);
            break;
        case 'S':
            uint8_t s_register = atoi(str);
            if (str[0] != '=')
                break;
            ++str;
            len -= 1;
            switch (s_register)
            {
            case 0:
                settings->auto_answer = atoi(str);
                break;
            case 2:
                settings->esc_char = atoi(str);
                break;
            case 3:
                settings->cr_char = atoi(str);
                break;
            case 4:
                settings->lf_char = atoi(str);
                break;
            case 5:
                settings->bs_char = atoi(str);
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }
    }
    lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfsresult < 0)
    {
        DBG("?Unable to lfs_file_close %s (%d)\n", modem0_sys, lfsresult);
        return false;
    }
    return true;
}

void mdm_task()
{
    if (!mdm_in_command_mode && mdm_tx_buf_len)
    {
        if (tel_tx(mdm_tx_buf, mdm_tx_buf_len))
            mdm_tx_buf_len = 0;
    }
    if (mdm_is_parsing)
    {
        if (mdm_response_state >= 0)
            return;
        if (!mdm_parse_result)
        {
            mdm_is_parsing = false;
            mdm_set_response_fn(mdm_response_code, 4); // ERROR
        }
        else if (*mdm_parse_str == 0)
        {
            mdm_is_parsing = false;
            if (mdm_in_command_mode)
                mdm_set_response_fn(mdm_response_code, 0); // OK
        }
        else
        {
            mdm_parse_result = cmd_parse(&mdm_parse_str);
        }
    }
    if (mdm_escape_count == MDM_ESCAPE_COUNT &&
        absolute_time_diff_us(get_absolute_time(), mdm_escape_guard) < 0)
    {
        mdm_in_command_mode = true;
        mdm_cmd_buf_len = 0;
        mdm_escape_count = 0;
        mdm_set_response_fn(mdm_response_code, 0); // OK
    }
}

bool mdm_dial(const char *s)
{
    if (mdm_state != mdm_state_on_hook)
        return false;
    // Use mbuf to slice the string.
    if (strlen(s) >= MBUF_SIZE)
        return false;
    char *buf = (char *)mbuf;
    strcpy(buf, s);
    u16_t port;
    char *port_str = strrchr(buf, ':');
    if (!port_str)
        port = 23;
    else
    {
        port_str++[0] = 0;
        port = atoi(port_str);
    }
    if (tel_open(buf, port))
    {
        mdm_state = mdm_state_dialing;
        mdm_in_command_mode = false;
        return true;
    }
    return false;
}

bool mdm_connect(void)
{
    if (mdm_state == mdm_state_dialing ||
        mdm_state == mdm_state_connected)
    {
        if (mdm_settings.progress > 0)
            mdm_set_response_fn(mdm_response_code, 5); // CONNECT 1200
        else
            mdm_set_response_fn(mdm_response_code, 1); // CONNECT
        mdm_state = mdm_state_connected;
        mdm_in_command_mode = false;
        return true;
    }
    return false;
}

bool mdm_hangup(void)
{
    if (mdm_state != mdm_state_on_hook)
    {
        mdm_set_response_fn(mdm_response_code, 3); // NO CARRIER
        mdm_state = mdm_state_on_hook;
        mdm_in_command_mode = true;
        tel_close();
        return true;
    }
    return false;
}

void mdm_carrier_lost(void)
{
    // If the telephone connection is lost while
    // we are escaped into command mode, hang up.
    if (mdm_in_command_mode)
        mdm_hangup();
}

#endif /* RP6502_RIA_W */
