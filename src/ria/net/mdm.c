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
bool mdm_std_handles(const char *) { return false; }
int mdm_std_open(const char *, uint8_t, api_errno *) { return -1; }
int mdm_std_close(int, api_errno *) { return -1; }
std_rw_result mdm_std_read(int, char *, uint32_t, uint32_t *, api_errno *) { return STD_ERROR; }
std_rw_result mdm_std_write(int, const char *, uint32_t, uint32_t *, api_errno *) { return STD_ERROR; }
#else

#include "net/cmd.h"
#include "net/mdm.h"
#include "net/tel.h"
#include "str/str.h"
#include "sys/lfs.h"
#include "sys/mem.h"
#include <pico/time.h>
#include <stdlib.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_MDM)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define MDM_ESCAPE_GUARD_TIME_US 1000000
#define MDM_ESCAPE_COUNT 3

// Old modems have 40 chars, Hayes V.series has 255.
#define MDM_AT_COMMAND_LEN (255)
static char mdm_cmd_buf[MDM_AT_COMMAND_LEN + 1];
static size_t mdm_cmd_buf_len;

#define MDM_RESPONSE_BUF_SIZE 128
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

static const char *const __in_flash("MDM_RESPONSES") MDM_RESPONSES[] = {
    STR_MDM_RESPONSE_0, STR_MDM_RESPONSE_1, STR_MDM_RESPONSE_2,
    STR_MDM_RESPONSE_3, STR_MDM_RESPONSE_4, STR_MDM_RESPONSE_5,
    STR_MDM_RESPONSE_6, STR_MDM_RESPONSE_7, STR_MDM_RESPONSE_8};

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

int mdm_response_code(char *buf, size_t buf_size, int state)
{
    assert(state >= 0 && (unsigned)state < sizeof(MDM_RESPONSES) / sizeof(char *));
    // X register result code availability bitmasks.
    // X0: 0-4, X1: 0-5, X2: 0-6, X3: 0-5 & 7, X4: 0-7.
    // Code 8 (NO ANSWER) is always available (@ dial modifier).
    static const uint16_t x_mask[] = {
        0x011F, // X0
        0x013F, // X1
        0x017F, // X2
        0x01BF, // X3
        0x01FF, // X4
    };
    unsigned x = mdm_settings.progress;
    if (x > 4)
        x = 4;
    bool suppress = false;
    if (mdm_settings.quiet == 1)
        suppress = true;
    // TODO quiet == 2 is different when answering
    else if (mdm_settings.quiet == 2 && state == 2)
        suppress = true;
    else if (!(x_mask[x] & (1u << state)))
        suppress = true;
    if (suppress)
        buf[0] = 0;
    else if (mdm_settings.verbose)
        snprintf(buf, buf_size, "\r\n%s\r\n", MDM_RESPONSES[state]);
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
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, STR_MDM_PHONEBOOK,
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
        DBG("?Unable to lfs_file_close %s (%d)\n", STR_MDM_PHONEBOOK, lfsresult);
    if (index)
        mbuf[0] = 0;
    return (char *)mbuf;
}

bool mdm_write_phonebook_entry(const char *entry, unsigned index)
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, STR_MDM_PHONE_TMP,
                                     LFS_O_RDWR | LFS_O_CREAT | LFS_O_TRUNC,
                                     &lfs_file_config);
    if (lfsresult < 0)
    {
        DBG("?Unable to lfs_file_opencfg %s for writing (%d)\n", STR_MDM_PHONE_TMP, lfsresult);
        return false;
    }
    for (unsigned i = 0; i < MDM_PHONEBOOK_ENTRIES; i++)
    {
        if (i == index)
            lfsresult = lfs_printf(&lfs_volume, &lfs_file, "%s\n", entry);
        else
            lfsresult = lfs_printf(&lfs_volume, &lfs_file, "%s\n", mdm_read_phonebook_entry(i));
        if (lfsresult < 0)
            DBG("?Unable to write %s contents (%d)\n", STR_MDM_PHONE_TMP, lfsresult);
    }
    int lfscloseresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfscloseresult < 0)
        DBG("?Unable to lfs_file_close %s (%d)\n", STR_MDM_PHONE_TMP, lfscloseresult);
    if (lfsresult < 0 || lfscloseresult < 0)
    {
        lfs_remove(&lfs_volume, STR_MDM_PHONE_TMP);
        return false;
    }
    lfsresult = lfs_remove(&lfs_volume, STR_MDM_PHONEBOOK);
    if (lfsresult < 0 && lfsresult != LFS_ERR_NOENT)
    {
        DBG("?Unable to lfs_remove %s (%d)\n", STR_MDM_PHONEBOOK, lfsresult);
        return false;
    }
    lfsresult = lfs_rename(&lfs_volume, STR_MDM_PHONE_TMP, STR_MDM_PHONEBOOK);
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
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, STR_MDM_SETTINGS,
                                     LFS_O_RDWR | LFS_O_CREAT,
                                     &lfs_file_config);
    if (lfsresult < 0)
        DBG("?Unable to lfs_file_opencfg %s for writing (%d)\n", STR_MDM_SETTINGS, lfsresult);
    if (lfsresult >= 0)
        if ((lfsresult = lfs_file_truncate(&lfs_volume, &lfs_file, 0)) < 0)
            DBG("?Unable to lfs_file_truncate %s (%d)\n", STR_MDM_SETTINGS, lfsresult);
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
            DBG("?Unable to write %s contents (%d)\n", STR_MDM_SETTINGS, lfsresult);
    }
    int lfscloseresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfscloseresult < 0)
        DBG("?Unable to lfs_file_close %s (%d)\n", STR_MDM_SETTINGS, lfscloseresult);
    if (lfsresult < 0 || lfscloseresult < 0)
    {
        lfs_remove(&lfs_volume, STR_MDM_SETTINGS);
        return false;
    }
    return true;
}

bool mdm_read_settings(mdm_settings_t *settings)
{
    mdm_factory_settings(settings);
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, STR_MDM_SETTINGS,
                                     LFS_O_RDONLY, &lfs_file_config);
    mbuf[0] = 0;
    if (lfsresult < 0)
    {
        if (lfsresult == LFS_ERR_NOENT)
            return true;
        DBG("?Unable to lfs_file_opencfg %s for reading (%d)\n", STR_MDM_SETTINGS, lfsresult);
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
            while (*str >= '0' && *str <= '9')
                str++;
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
        DBG("?Unable to lfs_file_close %s (%d)\n", STR_MDM_SETTINGS, lfsresult);
        return false;
    }
    return true;
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
    uint16_t port;
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
        mdm_state = mdm_state_on_hook;
        mdm_in_command_mode = true;
        tel_close();
        return true;
    }
    return false;
}

void mdm_carrier_lost(void)
{
    if (mdm_state != mdm_state_on_hook)
    {
        mdm_hangup();
        mdm_set_response_fn(mdm_response_code, 3); // NO CARRIER
    }
}

void mdm_init(void)
{
    mdm_stop();
}

void mdm_task()
{
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

void mdm_stop(void)
{
    tel_close();
    mdm_is_open = false;
    mdm_cmd_buf_len = 0;
    mdm_response_buf_head = 0;
    mdm_response_buf_tail = 0;
    mdm_response_state = -1;
    mdm_parse_result = true;
    mdm_state = mdm_state_on_hook;
    mdm_in_command_mode = true;
    mdm_is_parsing = false;
    mdm_escape_count = 0;
}

static void mdm_translate_newlines(void)
{
    size_t out = 0;
    for (size_t i = 0; i < mdm_response_buf_head; i++)
    {
        uint8_t ch = mdm_response_buf[i];
        bool translated = false;
        if (ch == '\r')
        {
            ch = mdm_settings.cr_char;
            translated = true;
        }
        else if (ch == '\n')
        {
            ch = mdm_settings.lf_char;
            translated = true;
        }
        if (!translated || !(ch & 0x80))
            mdm_response_buf[out++] = ch;
    }
    mdm_response_buf_head = out;
}

bool mdm_std_handles(const char *filename)
{
    return !strncasecmp(filename, "AT:", 3);
}

int mdm_std_open(const char *path, uint8_t flags, api_errno *err)
{
    (void)flags;
    if (mdm_is_open)
    {
        *err = API_EBUSY;
        return -1;
    }
    const char *filename = path;
    if (!strncasecmp(filename, "AT:", 3))
        filename += 3;
    else
    {
        *err = API_ENOENT;
        return -1;
    }
    mdm_read_settings(&mdm_settings);
    mdm_is_open = true;
    // Optionally process filename as AT command
    // after NVRAM read. e.g. AT:&F
    if (filename[0])
    {
        mdm_is_parsing = true;
        mdm_parse_result = true;
        snprintf(mdm_cmd_buf, sizeof(mdm_cmd_buf), "%s", filename);
        mdm_parse_str = mdm_cmd_buf;
    }
    return 0;
}

int mdm_std_close(int desc, api_errno *err)
{
    (void)desc;
    if (!mdm_is_open)
    {
        *err = API_EBADF;
        return -1;
    }
    mdm_stop();
    return 0;
}

std_rw_result mdm_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    (void)desc;
    if (!mdm_is_open)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    uint32_t pos = 0;
    while (pos < count)
    {
        // Refill response buffer from generator if needed
        if (mdm_response_buf_empty() && mdm_response_state >= 0)
        {
            mdm_response_state = mdm_response_fn(mdm_response_buf, MDM_RESPONSE_BUF_SIZE, mdm_response_state);
            mdm_response_buf_head = strlen(mdm_response_buf);
            mdm_response_buf_tail = 0;
            mdm_translate_newlines();
        }
        if (!mdm_response_buf_empty())
        {
            // Drain response buffer
            buf[pos++] = mdm_response_buf[mdm_response_buf_tail];
            mdm_response_buf_tail = (mdm_response_buf_tail + 1) % MDM_RESPONSE_BUF_SIZE;
        }
        else if (!mdm_in_command_mode)
        {
            // Read from telephone connection in data mode
            pos += tel_rx(&buf[pos], (uint16_t)(count - pos));
            break;
        }
        else
            break;
    }
    *bytes_read = pos;
    return STD_OK;
}

std_rw_result mdm_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    (void)desc;
    if (!mdm_is_open)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    if (mdm_is_parsing)
    {
        *bytes_written = 0;
        return STD_OK;
    }
    if (mdm_in_command_mode)
    {
        uint32_t pos = 0;
        while (pos < count)
        {
            if (!mdm_tx_command_mode(buf[pos]))
                break;
            pos++;
        }
        *bytes_written = pos;
        return STD_OK;
    }
    uint16_t bw = count;
    if (mdm_state == mdm_state_connected)
        bw = tel_tx(buf, bw);
    for (uint16_t i = 0; i < bw; i++)
        mdm_tx_escape_observer(buf[i]);
    *bytes_written = bw;
    return STD_OK;
}

#endif /* RP6502_RIA_W */
