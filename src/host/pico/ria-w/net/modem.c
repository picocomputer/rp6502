/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */



#include "ria-w/net/cmd.h"
#include "ria-w/net/modem.h"
#include "core/sys/config.h"
#include "ria-w/net/net.h"
#include "ria-w/net/cyw.h"
#include "ria-w/net/telnet.h"
#include "ria-w/net/wifi.h"
#include "core/str/str.h"
#include "ria/sys/com.h"
#include "osal/pico/lfs.h"
#include <pico/time.h>
#include <stdlib.h>

#if defined(DEBUG_NET) || defined(DEBUG_NET_MODEM)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define MODEM_ESCAPE_GUARD_TIME_US 1000000
#define MODEM_ESCAPE_COUNT 3

// Old modems have 40 chars, Hayes V.series has 255.
#define MODEM_AT_COMMAND_LEN (255)

#define MODEM_RESPONSE_BUF_SIZE 128

// Single queued response, rendered without word-wrap: the responders self-format
// and emit explicit newlines. 82 = 80 columns + newline + null.
#define MODEM_RESPONSE_WIDTH 80
#define MODEM_RESPONSE_RENDER_SIZE (MODEM_RESPONSE_WIDTH + 2)

typedef enum
{
    modem_state_on_hook,
    modem_state_ringing,
    modem_state_wait,
    modem_state_dialing,
    modem_state_connected,
    modem_state_disconnecting,
} modem_state_t;

typedef struct
{
    modem_state_t state;
    bool in_command_mode;
    bool is_open;
    int settings_slot;
    modem_settings_t settings;
    char cmd_buf[MODEM_AT_COMMAND_LEN + 1];
    size_t cmd_buf_len;
    char response_buf[MODEM_RESPONSE_BUF_SIZE]; // raw character-echo ring
    size_t response_buf_head;
    size_t response_buf_tail;
    modem_response_fn resp_fn; // single queued response source
    int resp_state;          // <0 when no active generator
    char resp_buf[MODEM_RESPONSE_RENDER_SIZE];
    size_t resp_len;
    size_t resp_pos;
    bool resp_prev_cr; // modem_sink: a CR was just emitted (suppress the lone-LF CR)
    bool parse_active;
    const char *parse_str;
    bool parse_result;
    uint16_t dial_port;
    unsigned escape_count;
    absolute_time_t escape_last_char;
    absolute_time_t escape_guard;
    uint8_t ring_count;
    absolute_time_t ring_timer;
    uint16_t active_listen_port;
    bool is_answering;
} modem_conn_t;

static modem_conn_t modem_conns[NET_MODEM_DESCS];
static modem_conn_t *modem_conn;

static char modem_phone_buf[MODEM_AT_COMMAND_LEN + 1];

static const char *const __in_flash("MODEM_RESPONSES") MODEM_RESPONSES[] = {
    STR_MODEM_RESPONSE_0, STR_MODEM_RESPONSE_1, STR_MODEM_RESPONSE_2,
    STR_MODEM_RESPONSE_3, STR_MODEM_RESPONSE_4, STR_MODEM_RESPONSE_5,
    STR_MODEM_RESPONSE_6, STR_MODEM_RESPONSE_7, STR_MODEM_RESPONSE_8};

static int modem_desc(void)
{
    return (int)(modem_conn - modem_conns);
}

void modem_set_conn(int desc)
{
    modem_conn = &modem_conns[desc];
}

modem_settings_t *modem_settings(void)
{
    return &modem_conn->settings;
}

bool modem_settings_persistent(void)
{
    return modem_conn->settings_slot >= 0;
}

static inline bool modem_response_buf_empty(void)
{
    return modem_conn->response_buf_head == modem_conn->response_buf_tail;
}

static inline bool modem_response_buf_full(void)
{
    return ((modem_conn->response_buf_head + 1) % MODEM_RESPONSE_BUF_SIZE) == modem_conn->response_buf_tail;
}

// A response is in progress while the generator is live or the render buffer
// still holds undrained bytes.
static bool modem_resp_busy(void)
{
    return modem_conn->resp_state >= 0 || modem_conn->resp_pos < modem_conn->resp_len;
}

static void modem_resp_reset(void)
{
    if (modem_conn->resp_fn && modem_conn->resp_state >= 0)
        modem_conn->resp_fn(modem_conn->resp_buf, MODEM_RESPONSE_RENDER_SIZE, -1,
                            MODEM_RESPONSE_WIDTH);
    modem_conn->resp_fn = NULL;
    modem_conn->resp_state = -1;
    modem_conn->resp_len = 0;
    modem_conn->resp_pos = 0;
    modem_conn->resp_prev_cr = false;
}

void modem_set_response_fn(modem_response_fn fn)
{
    modem_conn->resp_fn = fn;
    modem_conn->resp_state = 0;
    modem_conn->resp_len = 0;
    modem_conn->resp_pos = 0;
}

void modem_set_response_fn_state(modem_response_fn fn, int state)
{
    modem_conn->resp_fn = fn;
    modem_conn->resp_state = state;
    modem_conn->resp_len = 0;
    modem_conn->resp_pos = 0;
}

static void modem_response_append(char ch)
{
    if (!modem_response_buf_full())
    {
        modem_conn->response_buf[modem_conn->response_buf_head] = ch;
        modem_conn->response_buf_head = (modem_conn->response_buf_head + 1) % MODEM_RESPONSE_BUF_SIZE;
    }
}

static void modem_response_append_cr_lf(void)
{
    if (!(modem_conn->settings.cr_char & 0x80))
        modem_response_append(modem_conn->settings.cr_char);
    if (!(modem_conn->settings.lf_char & 0x80))
        modem_response_append(modem_conn->settings.lf_char);
}

static bool modem_cmd_buf_is_at_command(void)
{
    return (modem_conn->cmd_buf[0] == 'a' || modem_conn->cmd_buf[0] == 'A') &&
           (modem_conn->cmd_buf[1] == 't' || modem_conn->cmd_buf[1] == 'T');
}

static int modem_tx_command_mode(char ch)
{
    if (modem_resp_busy())
        return 0;
    if (!(modem_conn->settings.cr_char & 0x80) && ch == modem_conn->settings.cr_char)
    {
        if (modem_conn->settings.echo)
            modem_response_append_cr_lf();
        modem_conn->cmd_buf[modem_conn->cmd_buf_len] = 0;
        modem_conn->cmd_buf_len = 0;
        if (modem_cmd_buf_is_at_command())
        {
            modem_conn->parse_active = true;
            modem_conn->parse_result = true;
            modem_conn->parse_str = &modem_conn->cmd_buf[2];
        }
    }
    else if (ch == 127 || (!(modem_conn->settings.bs_char & 0x80) && ch == modem_conn->settings.bs_char))
    {
        if (modem_conn->settings.echo)
        {
            modem_response_append(modem_conn->settings.bs_char);
            modem_response_append(' ');
            modem_response_append(modem_conn->settings.bs_char);
        }
        if (modem_conn->cmd_buf_len)
            modem_conn->cmd_buf[--modem_conn->cmd_buf_len] = 0;
    }
    else if (ch >= 32 && ch < 127)
    {
        if (modem_conn->settings.echo)
            modem_response_append(ch);
        if (ch == '/' && modem_conn->cmd_buf_len == 1 && modem_cmd_buf_is_at_command())
        {
            if (modem_conn->settings.echo || (!modem_conn->settings.quiet && modem_conn->settings.verbose))
                modem_response_append_cr_lf();
            modem_conn->cmd_buf_len = 0;
            modem_conn->parse_active = true;
            modem_conn->parse_result = true;
            modem_conn->parse_str = &modem_conn->cmd_buf[2];
            return 1;
        }
        if (modem_conn->cmd_buf_len < MODEM_AT_COMMAND_LEN)
            modem_conn->cmd_buf[modem_conn->cmd_buf_len++] = ch;
    }
    return 1;
}

static void modem_tx_escape_observer(char ch)
{
    // S2 disabled: clear any stale count so re-enabling doesn't fire on a
    // partial old sequence.
    if (modem_conn->settings.esc_char >= 128)
    {
        modem_conn->escape_count = 0;
        modem_conn->escape_last_char = get_absolute_time();
        return;
    }
    bool last_char_guarded = time_reached(delayed_by_us(modem_conn->escape_last_char,
                                                        MODEM_ESCAPE_GUARD_TIME_US));
    if (modem_conn->escape_count && last_char_guarded)
        modem_conn->escape_count = 0;
    if (modem_conn->escape_count || last_char_guarded)
    {
        if (ch != modem_conn->settings.esc_char)
            modem_conn->escape_count = 0;
        else if (++modem_conn->escape_count == MODEM_ESCAPE_COUNT)
            modem_conn->escape_guard = make_timeout_time_us(MODEM_ESCAPE_GUARD_TIME_US);
    }
    modem_conn->escape_last_char = get_absolute_time();
}

static int modem_response_code(char *buf, size_t buf_size, int code, unsigned width)
{
    (void)width;
    if (code < 0)
        return code; // cancelled before consumption
    assert((unsigned)code < sizeof(MODEM_RESPONSES) / sizeof(char *));
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
    unsigned x = modem_conn->settings.progress;
    if (x > 4)
        x = 4;
    bool suppress = false;
    if (modem_conn->settings.quiet == 1)
        suppress = true;
    else if (modem_conn->settings.quiet == 2 && modem_conn->is_answering)
        suppress = true;
    else if (!(x_mask[x] & (1u << code)))
        suppress = true;
    if (suppress)
        buf[0] = 0;
    else if (modem_conn->settings.verbose)
        snprintf(buf, buf_size, "\r\n%s\r\n", MODEM_RESPONSES[code]);
    else
        snprintf(buf, buf_size, "%d\r", code);
    return -1;
}

void modem_factory_settings(modem_settings_t *settings)
{
    settings->s_pointer = 0;   // selected S-register; transient
    settings->echo = 1;        // E1
    settings->quiet = 0;       // Q0
    settings->verbose = 1;     // V1
    settings->progress = 0;    // X0
    settings->auto_answer = 0; // S0=0
    settings->esc_char = '+';  // S2=43
    settings->cr_char = '\r';  // S3=13
    settings->lf_char = '\n';  // S4=10
    settings->bs_char = '\b';  // S5=8
    settings->net_mode = 1;    // \N1
    settings->listen_port = 0; // \L0
    strcpy(settings->tty_type, "ANSI");
}

const char *modem_read_phonebook_entry(unsigned index)
{
    modem_phone_buf[0] = 0;
    if (modem_conn->settings_slot < 0)
        return modem_phone_buf;
    char phonebook_file[STR_MODEM_PHONEBOOK_LEN];
    snprintf(phonebook_file, sizeof(phonebook_file), STR_MODEM_PHONEBOOK, modem_conn->settings_slot);
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, phonebook_file,
                                     LFS_O_RDONLY, &lfs_file_config);
    if (lfsresult < 0)
        return modem_phone_buf;
    for (; lfs_gets(modem_phone_buf, sizeof(modem_phone_buf), &lfs_volume, &lfs_file, NULL); index--)
    {
        size_t len = strlen(modem_phone_buf);
        while (len && modem_phone_buf[len - 1] == '\n')
            len--;
        modem_phone_buf[len] = 0;
        if (index == 0)
            break;
    }
    lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfsresult < 0)
        DBG("?Unable to lfs_file_close %s (%d)\n", phonebook_file, lfsresult);
    if (index)
        modem_phone_buf[0] = 0;
    return modem_phone_buf;
}

bool modem_write_phonebook_entry(const char *entry, unsigned index)
{
    if (modem_conn->settings_slot < 0)
        return false;
    char phonebook_file[STR_MODEM_PHONEBOOK_LEN];
    char phone_tmp_file[STR_MODEM_PHONE_TMP_LEN];
    snprintf(phonebook_file, sizeof(phonebook_file), STR_MODEM_PHONEBOOK, modem_conn->settings_slot);
    snprintf(phone_tmp_file, sizeof(phone_tmp_file), STR_MODEM_PHONE_TMP, modem_conn->settings_slot);
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, phone_tmp_file,
                                     LFS_O_RDWR | LFS_O_CREAT | LFS_O_TRUNC,
                                     &lfs_file_config);
    if (lfsresult < 0)
    {
        DBG("?Unable to lfs_file_opencfg %s for writing (%d)\n", phone_tmp_file, lfsresult);
        return false;
    }
    for (unsigned i = 0; i < MODEM_PHONEBOOK_ENTRIES; i++)
    {
        if (i == index)
            lfsresult = lfs_printf(&lfs_volume, &lfs_file, "%s\n", entry);
        else
            lfsresult = lfs_printf(&lfs_volume, &lfs_file, "%s\n", modem_read_phonebook_entry(i));
        if (lfsresult < 0)
            DBG("?Unable to write %s contents (%d)\n", phone_tmp_file, lfsresult);
    }
    int lfscloseresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfscloseresult < 0)
        DBG("?Unable to lfs_file_close %s (%d)\n", phone_tmp_file, lfscloseresult);
    if (lfsresult < 0 || lfscloseresult < 0)
    {
        lfs_remove(&lfs_volume, phone_tmp_file);
        return false;
    }
    lfsresult = lfs_remove(&lfs_volume, phonebook_file);
    if (lfsresult < 0 && lfsresult != LFS_ERR_NOENT)
    {
        DBG("?Unable to lfs_remove %s (%d)\n", phonebook_file, lfsresult);
        return false;
    }
    lfsresult = lfs_rename(&lfs_volume, phone_tmp_file, phonebook_file);
    if (lfsresult < 0)
    {
        DBG("?Unable to lfs_rename (%d)\n", lfsresult);
        return false;
    }
    return true;
}

bool modem_write_settings(const modem_settings_t *settings)
{
    if (modem_conn->settings_slot < 0)
        return false;
    char settings_file[STR_MODEM_SETTINGS_LEN];
    snprintf(settings_file, sizeof(settings_file), STR_MODEM_SETTINGS, modem_conn->settings_slot);
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, settings_file,
                                     LFS_O_RDWR | LFS_O_CREAT | LFS_O_TRUNC,
                                     &lfs_file_config);
    if (lfsresult < 0)
        DBG("?Unable to lfs_file_opencfg %s for writing (%d)\n", settings_file, lfsresult);
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
                               "N=%u\n"
                               "L=%u\n"
                               "T=%s\n"
                               "",
                               settings->echo,
                               settings->quiet,
                               settings->verbose,
                               settings->progress,
                               settings->auto_answer,
                               settings->esc_char,
                               settings->cr_char,
                               settings->lf_char,
                               settings->bs_char,
                               settings->net_mode,
                               settings->listen_port,
                               settings->tty_type);
        if (lfsresult < 0)
            DBG("?Unable to write %s contents (%d)\n", settings_file, lfsresult);
    }
    int lfscloseresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfscloseresult < 0)
        DBG("?Unable to lfs_file_close %s (%d)\n", settings_file, lfscloseresult);
    if (lfsresult < 0 || lfscloseresult < 0)
    {
        lfs_remove(&lfs_volume, settings_file);
        return false;
    }
    return true;
}

bool modem_read_settings(modem_settings_t *settings)
{
    modem_factory_settings(settings);
    if (modem_conn->settings_slot < 0)
        return true;
    char settings_file[STR_MODEM_SETTINGS_LEN];
    snprintf(settings_file, sizeof(settings_file), STR_MODEM_SETTINGS, modem_conn->settings_slot);
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, settings_file,
                                     LFS_O_RDONLY, &lfs_file_config);
    if (lfsresult < 0)
    {
        if (lfsresult == LFS_ERR_NOENT)
            return true;
        DBG("?Unable to lfs_file_opencfg %s for reading (%d)\n", settings_file, lfsresult);
        return false;
    }
    char line[MODEM_AT_COMMAND_LEN + 1];
    while (lfs_gets(line, sizeof(line), &lfs_volume, &lfs_file, NULL))
    {
        size_t len = strlen(line);
        while (len && line[len - 1] == '\n')
            len--;
        line[len] = 0;
        const char *str = line + 1;
        len -= 1;
        switch (line[0])
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
        {
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
        }
        case 'L':
            if (str[0] == '=')
                settings->listen_port = atoi(str + 1);
            break;
        case 'N':
            if (str[0] == '=')
                settings->net_mode = atoi(str + 1);
            break;
        case 'T':
            if (str[0] == '=')
            {
                strncpy(settings->tty_type, str + 1, sizeof(settings->tty_type) - 1);
                settings->tty_type[sizeof(settings->tty_type) - 1] = 0;
            }
            break;
        default:
            break;
        }
    }
    lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfsresult < 0)
    {
        DBG("?Unable to lfs_file_close %s (%d)\n", settings_file, lfsresult);
        return false;
    }
    return true;
}

bool modem_dial(const char *s)
{
    if (modem_conn->state != modem_state_on_hook &&
        modem_conn->state != modem_state_ringing)
        return false;
    if (modem_conn->state == modem_state_ringing)
    {
        telnet_reject(modem_conn->settings.listen_port);
        modem_conn->ring_count = 0;
        modem_conn->state = modem_state_on_hook;
    }
    if (strlen(s) >= MODEM_AT_COMMAND_LEN)
        return false;
    char buf[MODEM_AT_COMMAND_LEN + 1];
    strcpy(buf, s);
    uint16_t port;
    char *port_str = strrchr(buf, ':');
    if (!port_str)
        port = 23;
    else
    {
        *port_str = '\0';
        port_str++;
        port = atoi(port_str);
    }
    modem_conn->parse_active = false;
    modem_conn->is_answering = false;
    strcpy(modem_conn->cmd_buf, buf);
    modem_conn->dial_port = port;
    modem_conn->state = modem_state_wait;
    modem_conn->in_command_mode = false;
    return true;
}

bool modem_connect(void)
{
    // ATO on an already-established connection: just re-enter data mode.
    // No CONNECT response — otherwise a later async tcp_connect completion
    // would emit a second CONNECT.
    if (modem_conn->state == modem_state_connected)
    {
        modem_conn->in_command_mode = false;
        return true;
    }
    if (modem_conn->state == modem_state_dialing)
    {
        if (modem_conn->settings.progress > 0)
            modem_set_response_fn_state(modem_response_code, 5); // CONNECT 1200
        else
            modem_set_response_fn_state(modem_response_code, 1); // CONNECT
        modem_conn->state = modem_state_connected;
        modem_conn->in_command_mode = false;
        telnet_negotiate(modem_desc(), modem_conn->settings.net_mode != 0,
                      modem_conn->settings.tty_type);
        return true;
    }
    return false;
}

bool modem_hangup(void)
{
    if (modem_conn->state == modem_state_ringing)
    {
        telnet_reject(modem_conn->settings.listen_port);
        modem_conn->state = modem_state_on_hook;
        modem_conn->in_command_mode = true;
        modem_conn->ring_count = 0;
        return true;
    }
    if (modem_conn->state != modem_state_on_hook)
    {
        modem_conn->state = modem_state_on_hook;
        modem_conn->in_command_mode = true;
        telnet_close(modem_desc());
        return true;
    }
    return false;
}

static void modem_finalize_carrier_lost(void)
{
    modem_hangup();
    modem_resp_reset();
    modem_set_response_fn_state(modem_response_code, 3); // NO CARRIER
}

static void modem_carrier_lost(void)
{
    if (modem_conn->state == modem_state_on_hook)
        return;
    DBG("NET MODEM carrier lost\n");
    // Remote FIN while DTE is in data mode: defer NO CARRIER until
    // modem_std_read has drained net's buffered pbufs. net is already in
    // net_state_closing and will self-close on drain.
    if (modem_conn->state == modem_state_connected && !modem_conn->in_command_mode)
    {
        modem_conn->state = modem_state_disconnecting;
        return;
    }
    modem_finalize_carrier_lost();
}

bool modem_conns_is_open(int desc)
{
    return modem_conns[desc].is_open;
}

uint16_t modem_conns_listen_port(int desc)
{
    return modem_conns[desc].settings.listen_port;
}

uint8_t modem_get_ring_count(void)
{
    return modem_conn->ring_count;
}

static void modem_ring(void)
{
    if (modem_conn->state != modem_state_on_hook)
        return;
    modem_conn->state = modem_state_ringing;
    modem_conn->ring_count = 0;
    modem_conn->ring_timer = get_absolute_time();
    modem_conn->is_answering = false;
}

static void modem_net_on_close(int desc)
{
    modem_set_conn(desc);
    modem_carrier_lost();
}

bool modem_answer(void)
{
    if (modem_conn->state != modem_state_ringing)
        return false;
    modem_conn->is_answering = true;
    if (!telnet_accept(modem_desc(), modem_conn->settings.listen_port,
                    modem_conn->settings.net_mode != 0, modem_conn->settings.tty_type,
                    modem_net_on_close))
    {
        // Call gone — answered elsewhere or remote hung up
        modem_conn->state = modem_state_on_hook;
        modem_conn->in_command_mode = true;
        modem_conn->ring_count = 0;
        modem_resp_reset();
        modem_set_response_fn_state(modem_response_code, 3); // NO CARRIER
        return true;
    }
    modem_conn->state = modem_state_connected;
    modem_conn->in_command_mode = false;
    modem_conn->ring_count = 0;
    modem_resp_reset();
    if (modem_conn->settings.progress > 0)
        modem_set_response_fn_state(modem_response_code, 5); // CONNECT 1200
    else
        modem_set_response_fn_state(modem_response_code, 1); // CONNECT
    return true;
}

static bool modem_net_on_accept(uint16_t port)
{
    // Only one modem takes the call; the rest stay on-hook.
    for (int i = 0; i < NET_MODEM_DESCS; i++)
    {
        if (!modem_conns[i].is_open)
            continue;
        if (modem_conns[i].settings.listen_port != port)
            continue;
        if (modem_conns[i].state != modem_state_on_hook)
            continue;
        modem_set_conn(i);
        modem_ring();
        return true;
    }
    return false;
}

static void modem_listen_update(void)
{
    uint16_t active = modem_conn->active_listen_port;
    uint16_t wanted = modem_conn->settings.listen_port;
    if (active == wanted)
        return;
    if (active > 0)
    {
        if (modem_conn->state == modem_state_ringing)
        {
            telnet_reject(active);
            modem_conn->state = modem_state_on_hook;
            modem_conn->ring_count = 0;
        }
        telnet_listen_close(active);
        modem_conn->active_listen_port = 0;
    }
    if (wanted == 0)
        return;
    if (wanted == com_telnet_get_port())
    {
        DBG("NET MODEM %d listen_port conflicts with console, reset to 0\n", modem_desc());
        modem_conn->settings.listen_port = 0;
        return;
    }
    if (!wifi_ready())
        return;
    if (telnet_listen(wanted, modem_net_on_accept))
    {
        modem_conn->active_listen_port = wanted;
        DBG("NET MODEM %d listening on port %u\n", modem_desc(), wanted);
    }
}

bool modem_set_listen_port(uint16_t port)
{
    if (port > 0 && port == com_telnet_get_port())
        return false;
    modem_conn->settings.listen_port = port;
    modem_listen_update();
    return true;
}

void __in_flash("modem_init") modem_init(void)
{
    modem_stop();
}

void modem_task()
{
    for (int i = 0; i < NET_MODEM_DESCS; i++)
    {
        if (!modem_conns[i].is_open)
            continue;
        modem_set_conn(i);
        if (modem_conn->parse_active)
        {
            if (modem_resp_busy())
                continue;
            if (!modem_conn->parse_result)
            {
                modem_conn->parse_active = false;
                modem_set_response_fn_state(modem_response_code, 4); // ERROR
            }
            else if (*modem_conn->parse_str == 0)
            {
                modem_conn->parse_active = false;
                if (modem_conn->in_command_mode)
                    modem_set_response_fn(modem_response_code); // OK
            }
            else
            {
                modem_conn->parse_result = cmd_parse(&modem_conn->parse_str);
            }
        }
        modem_listen_update();
        if (modem_conn->state == modem_state_ringing)
        {
            if (!telnet_has_pending(modem_conn->settings.listen_port))
            {
                // Call gone (answered elsewhere or remote hung up)
                modem_conn->state = modem_state_on_hook;
                modem_conn->in_command_mode = true;
                modem_conn->ring_count = 0;
                modem_resp_reset();
                modem_set_response_fn_state(modem_response_code, 3); // NO CARRIER
            }
            else if (time_reached(modem_conn->ring_timer) &&
                     !modem_resp_busy())
            {
                modem_conn->ring_count++;
                modem_set_response_fn_state(modem_response_code, 2); // RING
                modem_conn->ring_timer = make_timeout_time_ms(6000);
                if (modem_conn->settings.auto_answer > 0 &&
                    modem_conn->ring_count >= modem_conn->settings.auto_answer)
                {
                    modem_answer();
                }
            }
        }
        if (modem_conn->state == modem_state_wait)
        {
            if (wifi_ready())
            {
                if (telnet_open(modem_desc(), modem_conn->cmd_buf, modem_conn->dial_port,
                             modem_net_on_close))
                    modem_conn->state = modem_state_dialing;
                else
                {
                    DBG("NET MODEM dial failed after wifi ready\n");
                    modem_conn->state = modem_state_on_hook;
                    modem_conn->in_command_mode = true;
                    modem_set_response_fn_state(modem_response_code, 3); // NO CARRIER
                }
            }
            else if (!wifi_connecting())
            {
                DBG("NET MODEM dial failed, wifi not connecting\n");
                modem_conn->state = modem_state_on_hook;
                modem_conn->in_command_mode = true;
                modem_set_response_fn_state(modem_response_code, 3); // NO CARRIER
            }
        }
        if (modem_conn->escape_count == MODEM_ESCAPE_COUNT &&
            time_reached(modem_conn->escape_guard))
        {
            modem_conn->escape_count = 0;
            if (!modem_conn->in_command_mode)
            {
                modem_conn->cmd_buf_len = 0;
                modem_conn->in_command_mode = true;
                modem_resp_reset();
                modem_set_response_fn(modem_response_code); // OK
            }
        }
    }
}

static void modem_conn_stop(modem_conn_t *conn)
{
    modem_conn = conn;
    if (conn->active_listen_port > 0)
    {
        telnet_listen_close(conn->active_listen_port);
        conn->active_listen_port = 0;
    }
    telnet_close((int)(conn - modem_conns));
    conn->is_open = false;
    conn->cmd_buf_len = 0;
    conn->response_buf_head = 0;
    conn->response_buf_tail = 0;
    modem_resp_reset();
    conn->parse_result = true;
    conn->state = modem_state_on_hook;
    conn->in_command_mode = true;
    conn->parse_active = false;
    conn->escape_count = 0;
    conn->ring_count = 0;
    conn->is_answering = false;
}

void modem_stop(void)
{
    for (int i = 0; i < NET_MODEM_DESCS; i++)
        modem_conn_stop(&modem_conns[i]);
}

// Output stage for the response renderer: maps a canonical '\n' to the
// configured S3/S4 CR-LF (inserting CR before a lone LF, idempotent after an
// explicit '\r', honoring the high-bit disable) and writes into the active
// read. Returns false at the read's count so the render pauses.
static char *modem_sink_buf;
static uint32_t modem_sink_count;
static uint32_t modem_sink_pos;

static bool modem_sink(char ch)
{
    uint8_t cr = modem_conn->settings.cr_char;
    uint8_t lf = modem_conn->settings.lf_char;
    char out[2];
    int n = 0;
    if (ch == '\r')
    {
        if (!(cr & 0x80))
            out[n++] = (char)cr;
    }
    else if (ch == '\n')
    {
        if (!modem_conn->resp_prev_cr && !(cr & 0x80))
            out[n++] = (char)cr;
        if (!(lf & 0x80))
            out[n++] = (char)lf;
    }
    else
        out[n++] = ch;
    if (modem_sink_pos + (uint32_t)n > modem_sink_count)
        return false;
    for (int i = 0; i < n; i++)
        modem_sink_buf[modem_sink_pos++] = out[i];
    modem_conn->resp_prev_cr = (ch == '\r');
    return true;
}

bool modem_std_handles(const char *filename)
{
    if (strncasecmp(filename, "AT", 2) != 0)
        return false;
    if (filename[2] == ':')
        return true;
    if (filename[2] >= '0' && filename[2] <= '9' && filename[3] == ':')
        return true;
    return false;
}

int modem_std_open(const char *path, uint8_t flags, api_errno *err)
{
    (void)flags;
    if (!cyw_get_rf_enable())
    {
        *err = API_ENODEV;
        return -1;
    }
    int desc = -1;
    for (int i = 0; i < NET_MODEM_DESCS; i++)
    {
        if (!modem_conns[i].is_open)
        {
            desc = i;
            break;
        }
    }
    if (desc < 0)
    {
        *err = API_EBUSY;
        return -1;
    }
    modem_set_conn(desc);
    const char *filename = path;
    if (!strncasecmp(filename, "AT", 2))
    {
        filename += 2;
        if (*filename >= '0' && *filename <= '9')
        {
            modem_conn->settings_slot = *filename - '0';
            filename++;
        }
        else
            modem_conn->settings_slot = -1;
        if (*filename == ':')
            filename++;
        else
        {
            *err = API_ENOENT;
            return -1;
        }
    }
    else
    {
        *err = API_ENOENT;
        return -1;
    }
    if (modem_conn->settings_slot >= 0)
        modem_read_settings(&modem_conn->settings);
    else
        modem_factory_settings(&modem_conn->settings);
    modem_conn->is_open = true;
    modem_listen_update();
    // Optionally process filename as AT command
    // after NVRAM read. e.g. AT0:&F
    if (filename[0])
    {
        modem_conn->parse_active = true;
        modem_conn->parse_result = true;
        snprintf(modem_conn->cmd_buf, sizeof(modem_conn->cmd_buf), "%s", filename);
        modem_conn->parse_str = modem_conn->cmd_buf;
    }
    return desc;
}

std_rw_result modem_std_close(int desc, api_errno *err)
{
    if (desc < 0 || desc >= NET_MODEM_DESCS || !modem_conns[desc].is_open)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    modem_conn_stop(&modem_conns[desc]);
    return STD_OK;
}

std_rw_result modem_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    if (desc < 0 || desc >= NET_MODEM_DESCS || !modem_conns[desc].is_open)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    modem_set_conn(desc);
    uint32_t pos = 0;
    for (;;)
    {
        // Drain the raw character-echo ring first.
        if (!modem_response_buf_empty())
        {
            while (pos < count && !modem_response_buf_empty())
            {
                buf[pos++] = modem_conn->response_buf[modem_conn->response_buf_tail];
                modem_conn->response_buf_tail = (modem_conn->response_buf_tail + 1) % MODEM_RESPONSE_BUF_SIZE;
            }
            if (pos >= count)
                break;
        }
        // Render the queued response, S3/S4-translated. Responders self-format
        // to MODEM_RESPONSE_WIDTH and emit explicit newlines, so no word-wrap.
        if (modem_resp_busy())
        {
            if (pos >= count)
                break;
            // Refill the chunk when drained; an empty chunk with a live state is
            // an async await (e.g. a scan still running) — resume on a later read.
            if (modem_conn->resp_pos >= modem_conn->resp_len)
            {
                modem_conn->resp_buf[0] = 0;
                modem_conn->resp_state = modem_conn->resp_fn(modem_conn->resp_buf,
                                                             MODEM_RESPONSE_RENDER_SIZE,
                                                         modem_conn->resp_state,
                                                         MODEM_RESPONSE_WIDTH);
                modem_conn->resp_len = strlen(modem_conn->resp_buf);
                modem_conn->resp_pos = 0;
                if (modem_conn->resp_len == 0)
                {
                    if (modem_conn->resp_state >= 0)
                        break; // nothing yet; resume later
                    continue;  // generator done with no output
                }
            }
            modem_sink_buf = buf;
            modem_sink_count = count;
            modem_sink_pos = pos;
            while (modem_conn->resp_pos < modem_conn->resp_len)
            {
                if (!modem_sink(modem_conn->resp_buf[modem_conn->resp_pos]))
                    break;
                modem_conn->resp_pos++;
            }
            pos = modem_sink_pos;
            if (modem_conn->resp_pos < modem_conn->resp_len)
                break; // read buffer full; resume mid-chunk on a later read
            continue;
        }
        // Read from the telephone connection in data mode.
        if (!modem_conn->in_command_mode)
        {
            uint16_t got = telnet_rx(modem_desc(), &buf[pos], (uint16_t)(count - pos));
            pos += got;
            if (got == 0 && modem_conn->state == modem_state_disconnecting)
            {
                // Buffered RX drained after remote FIN; emit NO CARRIER now.
                modem_finalize_carrier_lost();
                continue;
            }
        }
        break;
    }
    *bytes_read = pos;
    return STD_OK;
}

std_rw_result modem_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    if (desc < 0 || desc >= NET_MODEM_DESCS || !modem_conns[desc].is_open)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    modem_set_conn(desc);
    if (modem_conn->parse_active)
    {
        *bytes_written = 0;
        return STD_OK;
    }
    if (modem_conn->in_command_mode)
    {
        uint32_t pos = 0;
        while (pos < count)
        {
            if (!modem_tx_command_mode(buf[pos]))
                break;
            pos++;
        }
        *bytes_written = pos;
        return STD_OK;
    }
    if (modem_conn->state != modem_state_connected)
    {
        // DTE flow control: no transport (dial in progress, carrier draining).
        // Mirrors a real modem holding CTS low.
        *bytes_written = 0;
        return STD_OK;
    }
    uint16_t bw = count > UINT16_MAX ? UINT16_MAX : (uint16_t)count;
    bw = telnet_tx(modem_desc(), buf, bw);
    for (uint16_t i = 0; i < bw; i++)
        modem_tx_escape_observer(buf[i]);
    *bytes_written = bw;
    return STD_OK;
}
