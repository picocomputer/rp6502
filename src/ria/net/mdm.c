/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W
#include "net/mdm.h"
#include <pico/stdlib.h>
void mdm_task(void) {}
void mdm_stop(void) {}
void __in_flash("mdm_init") mdm_init(void) {}
void mdm_set_conn(int desc) { (void)desc; }
bool mdm_settings_persistent(void) { return false; }
bool mdm_std_handles(const char *) { return false; }
int mdm_std_open(const char *, uint8_t, api_errno *) { return -1; }
std_rw_result mdm_std_close(int, api_errno *) { return STD_ERROR; }
std_rw_result mdm_std_read(int, char *, uint32_t, uint32_t *, api_errno *) { return STD_ERROR; }
std_rw_result mdm_std_write(int, const char *, uint32_t, uint32_t *, api_errno *) { return STD_ERROR; }
bool mdm_answer(void) { return false; }
uint8_t mdm_get_ring_count(void) { return 0; }
bool mdm_conns_is_open(int desc)
{
    (void)desc;
    return false;
}
uint16_t mdm_conns_listen_port(int desc)
{
    (void)desc;
    return 0;
}
bool mdm_set_listen_port(uint16_t port)
{
    (void)port;
    return false;
}
#else

#include "net/cmd.h"
#include "net/mdm.h"
#include "net/net.h"
#include "net/cyw.h"
#include "net/tel.h"
#include "net/wfi.h"
#include "str/str.h"
#include "sys/com_hw.h"
#include "sys/lfs.h"
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

#define MDM_RESPONSE_BUF_SIZE 128

// Single queued response, rendered without word-wrap: the responders self-format
// and emit explicit newlines. 82 = 80 columns + newline + null.
#define MDM_RESPONSE_WIDTH 80
#define MDM_RESPONSE_RENDER_SIZE (MDM_RESPONSE_WIDTH + 2)

typedef enum
{
    mdm_state_on_hook,
    mdm_state_ringing,
    mdm_state_wait,
    mdm_state_dialing,
    mdm_state_connected,
    mdm_state_disconnecting,
} mdm_state_t;

typedef struct
{
    mdm_state_t state;
    bool in_command_mode;
    bool is_open;
    int settings_slot;
    mdm_settings_t settings;
    char cmd_buf[MDM_AT_COMMAND_LEN + 1];
    size_t cmd_buf_len;
    char response_buf[MDM_RESPONSE_BUF_SIZE]; // raw character-echo ring
    size_t response_buf_head;
    size_t response_buf_tail;
    mdm_response_fn resp_fn; // single queued response source
    int resp_state;          // <0 when no active generator
    char resp_buf[MDM_RESPONSE_RENDER_SIZE];
    size_t resp_len;
    size_t resp_pos;
    bool resp_prev_cr; // mdm_sink: a CR was just emitted (suppress the lone-LF CR)
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
} mdm_conn_t;

static mdm_conn_t mdm_conns[NET_MDM_DESCS];
static mdm_conn_t *mdm_conn;

static char mdm_phone_buf[MDM_AT_COMMAND_LEN + 1];

static const char *const __in_flash("MDM_RESPONSES") MDM_RESPONSES[] = {
    STR_MDM_RESPONSE_0, STR_MDM_RESPONSE_1, STR_MDM_RESPONSE_2,
    STR_MDM_RESPONSE_3, STR_MDM_RESPONSE_4, STR_MDM_RESPONSE_5,
    STR_MDM_RESPONSE_6, STR_MDM_RESPONSE_7, STR_MDM_RESPONSE_8};

static int mdm_desc(void)
{
    return (int)(mdm_conn - mdm_conns);
}

void mdm_set_conn(int desc)
{
    mdm_conn = &mdm_conns[desc];
}

mdm_settings_t *mdm_settings(void)
{
    return &mdm_conn->settings;
}

bool mdm_settings_persistent(void)
{
    return mdm_conn->settings_slot >= 0;
}

static inline bool mdm_response_buf_empty(void)
{
    return mdm_conn->response_buf_head == mdm_conn->response_buf_tail;
}

static inline bool mdm_response_buf_full(void)
{
    return ((mdm_conn->response_buf_head + 1) % MDM_RESPONSE_BUF_SIZE) == mdm_conn->response_buf_tail;
}

// A response is in progress while the generator is live or the render buffer
// still holds undrained bytes.
static bool mdm_resp_busy(void)
{
    return mdm_conn->resp_state >= 0 || mdm_conn->resp_pos < mdm_conn->resp_len;
}

static void mdm_resp_reset(void)
{
    if (mdm_conn->resp_fn && mdm_conn->resp_state >= 0)
        mdm_conn->resp_fn(mdm_conn->resp_buf, MDM_RESPONSE_RENDER_SIZE, -1,
                          MDM_RESPONSE_WIDTH);
    mdm_conn->resp_fn = NULL;
    mdm_conn->resp_state = -1;
    mdm_conn->resp_len = 0;
    mdm_conn->resp_pos = 0;
    mdm_conn->resp_prev_cr = false;
}

void mdm_set_response_fn(mdm_response_fn fn)
{
    mdm_conn->resp_fn = fn;
    mdm_conn->resp_state = 0;
    mdm_conn->resp_len = 0;
    mdm_conn->resp_pos = 0;
}

void mdm_set_response_fn_state(mdm_response_fn fn, int state)
{
    mdm_conn->resp_fn = fn;
    mdm_conn->resp_state = state;
    mdm_conn->resp_len = 0;
    mdm_conn->resp_pos = 0;
}

static void mdm_response_append(char ch)
{
    if (!mdm_response_buf_full())
    {
        mdm_conn->response_buf[mdm_conn->response_buf_head] = ch;
        mdm_conn->response_buf_head = (mdm_conn->response_buf_head + 1) % MDM_RESPONSE_BUF_SIZE;
    }
}

static void mdm_response_append_cr_lf(void)
{
    if (!(mdm_conn->settings.cr_char & 0x80))
        mdm_response_append(mdm_conn->settings.cr_char);
    if (!(mdm_conn->settings.lf_char & 0x80))
        mdm_response_append(mdm_conn->settings.lf_char);
}

static bool mdm_cmd_buf_is_at_command(void)
{
    return (mdm_conn->cmd_buf[0] == 'a' || mdm_conn->cmd_buf[0] == 'A') &&
           (mdm_conn->cmd_buf[1] == 't' || mdm_conn->cmd_buf[1] == 'T');
}

static int mdm_tx_command_mode(char ch)
{
    if (mdm_resp_busy())
        return 0;
    if (!(mdm_conn->settings.cr_char & 0x80) && ch == mdm_conn->settings.cr_char)
    {
        if (mdm_conn->settings.echo)
            mdm_response_append_cr_lf();
        mdm_conn->cmd_buf[mdm_conn->cmd_buf_len] = 0;
        mdm_conn->cmd_buf_len = 0;
        if (mdm_cmd_buf_is_at_command())
        {
            mdm_conn->parse_active = true;
            mdm_conn->parse_result = true;
            mdm_conn->parse_str = &mdm_conn->cmd_buf[2];
        }
    }
    else if (ch == 127 || (!(mdm_conn->settings.bs_char & 0x80) && ch == mdm_conn->settings.bs_char))
    {
        if (mdm_conn->settings.echo)
        {
            mdm_response_append(mdm_conn->settings.bs_char);
            mdm_response_append(' ');
            mdm_response_append(mdm_conn->settings.bs_char);
        }
        if (mdm_conn->cmd_buf_len)
            mdm_conn->cmd_buf[--mdm_conn->cmd_buf_len] = 0;
    }
    else if (ch >= 32 && ch < 127)
    {
        if (mdm_conn->settings.echo)
            mdm_response_append(ch);
        if (ch == '/' && mdm_conn->cmd_buf_len == 1 && mdm_cmd_buf_is_at_command())
        {
            if (mdm_conn->settings.echo || (!mdm_conn->settings.quiet && mdm_conn->settings.verbose))
                mdm_response_append_cr_lf();
            mdm_conn->cmd_buf_len = 0;
            mdm_conn->parse_active = true;
            mdm_conn->parse_result = true;
            mdm_conn->parse_str = &mdm_conn->cmd_buf[2];
            return 1;
        }
        if (mdm_conn->cmd_buf_len < MDM_AT_COMMAND_LEN)
            mdm_conn->cmd_buf[mdm_conn->cmd_buf_len++] = ch;
    }
    return 1;
}

static void mdm_tx_escape_observer(char ch)
{
    // S2 disabled: clear any stale count so re-enabling doesn't fire on a
    // partial old sequence.
    if (mdm_conn->settings.esc_char >= 128)
    {
        mdm_conn->escape_count = 0;
        mdm_conn->escape_last_char = get_absolute_time();
        return;
    }
    bool last_char_guarded = time_reached(delayed_by_us(mdm_conn->escape_last_char,
                                                        MDM_ESCAPE_GUARD_TIME_US));
    if (mdm_conn->escape_count && last_char_guarded)
        mdm_conn->escape_count = 0;
    if (mdm_conn->escape_count || last_char_guarded)
    {
        if (ch != mdm_conn->settings.esc_char)
            mdm_conn->escape_count = 0;
        else if (++mdm_conn->escape_count == MDM_ESCAPE_COUNT)
            mdm_conn->escape_guard = make_timeout_time_us(MDM_ESCAPE_GUARD_TIME_US);
    }
    mdm_conn->escape_last_char = get_absolute_time();
}

static int mdm_response_code(char *buf, size_t buf_size, int code, unsigned width)
{
    (void)width;
    if (code < 0)
        return code; // cancelled before consumption
    assert((unsigned)code < sizeof(MDM_RESPONSES) / sizeof(char *));
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
    unsigned x = mdm_conn->settings.progress;
    if (x > 4)
        x = 4;
    bool suppress = false;
    if (mdm_conn->settings.quiet == 1)
        suppress = true;
    else if (mdm_conn->settings.quiet == 2 && mdm_conn->is_answering)
        suppress = true;
    else if (!(x_mask[x] & (1u << code)))
        suppress = true;
    if (suppress)
        buf[0] = 0;
    else if (mdm_conn->settings.verbose)
        snprintf(buf, buf_size, "\r\n%s\r\n", MDM_RESPONSES[code]);
    else
        snprintf(buf, buf_size, "%d\r", code);
    return -1;
}

void mdm_factory_settings(mdm_settings_t *settings)
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

const char *mdm_read_phonebook_entry(unsigned index)
{
    mdm_phone_buf[0] = 0;
    if (mdm_conn->settings_slot < 0)
        return mdm_phone_buf;
    char phonebook_file[STR_MDM_PHONEBOOK_LEN];
    snprintf(phonebook_file, sizeof(phonebook_file), STR_MDM_PHONEBOOK, mdm_conn->settings_slot);
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, phonebook_file,
                                     LFS_O_RDONLY, &lfs_file_config);
    if (lfsresult < 0)
        return mdm_phone_buf;
    for (; lfs_gets(mdm_phone_buf, sizeof(mdm_phone_buf), &lfs_volume, &lfs_file, NULL); index--)
    {
        size_t len = strlen(mdm_phone_buf);
        while (len && mdm_phone_buf[len - 1] == '\n')
            len--;
        mdm_phone_buf[len] = 0;
        if (index == 0)
            break;
    }
    lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfsresult < 0)
        DBG("?Unable to lfs_file_close %s (%d)\n", phonebook_file, lfsresult);
    if (index)
        mdm_phone_buf[0] = 0;
    return mdm_phone_buf;
}

bool mdm_write_phonebook_entry(const char *entry, unsigned index)
{
    if (mdm_conn->settings_slot < 0)
        return false;
    char phonebook_file[STR_MDM_PHONEBOOK_LEN];
    char phone_tmp_file[STR_MDM_PHONE_TMP_LEN];
    snprintf(phonebook_file, sizeof(phonebook_file), STR_MDM_PHONEBOOK, mdm_conn->settings_slot);
    snprintf(phone_tmp_file, sizeof(phone_tmp_file), STR_MDM_PHONE_TMP, mdm_conn->settings_slot);
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
    for (unsigned i = 0; i < MDM_PHONEBOOK_ENTRIES; i++)
    {
        if (i == index)
            lfsresult = lfs_printf(&lfs_volume, &lfs_file, "%s\n", entry);
        else
            lfsresult = lfs_printf(&lfs_volume, &lfs_file, "%s\n", mdm_read_phonebook_entry(i));
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

bool mdm_write_settings(const mdm_settings_t *settings)
{
    if (mdm_conn->settings_slot < 0)
        return false;
    char settings_file[STR_MDM_SETTINGS_LEN];
    snprintf(settings_file, sizeof(settings_file), STR_MDM_SETTINGS, mdm_conn->settings_slot);
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

bool mdm_read_settings(mdm_settings_t *settings)
{
    mdm_factory_settings(settings);
    if (mdm_conn->settings_slot < 0)
        return true;
    char settings_file[STR_MDM_SETTINGS_LEN];
    snprintf(settings_file, sizeof(settings_file), STR_MDM_SETTINGS, mdm_conn->settings_slot);
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
    char line[MDM_AT_COMMAND_LEN + 1];
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

bool mdm_dial(const char *s)
{
    if (mdm_conn->state != mdm_state_on_hook &&
        mdm_conn->state != mdm_state_ringing)
        return false;
    if (mdm_conn->state == mdm_state_ringing)
    {
        tel_reject(mdm_conn->settings.listen_port);
        mdm_conn->ring_count = 0;
        mdm_conn->state = mdm_state_on_hook;
    }
    if (strlen(s) >= MDM_AT_COMMAND_LEN)
        return false;
    char buf[MDM_AT_COMMAND_LEN + 1];
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
    mdm_conn->parse_active = false;
    mdm_conn->is_answering = false;
    strcpy(mdm_conn->cmd_buf, buf);
    mdm_conn->dial_port = port;
    mdm_conn->state = mdm_state_wait;
    mdm_conn->in_command_mode = false;
    return true;
}

bool mdm_connect(void)
{
    // ATO on an already-established connection: just re-enter data mode.
    // No CONNECT response — otherwise a later async tcp_connect completion
    // would emit a second CONNECT.
    if (mdm_conn->state == mdm_state_connected)
    {
        mdm_conn->in_command_mode = false;
        return true;
    }
    if (mdm_conn->state == mdm_state_dialing)
    {
        if (mdm_conn->settings.progress > 0)
            mdm_set_response_fn_state(mdm_response_code, 5); // CONNECT 1200
        else
            mdm_set_response_fn_state(mdm_response_code, 1); // CONNECT
        mdm_conn->state = mdm_state_connected;
        mdm_conn->in_command_mode = false;
        tel_negotiate(mdm_desc(), mdm_conn->settings.net_mode != 0,
                      mdm_conn->settings.tty_type);
        return true;
    }
    return false;
}

bool mdm_hangup(void)
{
    if (mdm_conn->state == mdm_state_ringing)
    {
        tel_reject(mdm_conn->settings.listen_port);
        mdm_conn->state = mdm_state_on_hook;
        mdm_conn->in_command_mode = true;
        mdm_conn->ring_count = 0;
        return true;
    }
    if (mdm_conn->state != mdm_state_on_hook)
    {
        mdm_conn->state = mdm_state_on_hook;
        mdm_conn->in_command_mode = true;
        tel_close(mdm_desc());
        return true;
    }
    return false;
}

static void mdm_finalize_carrier_lost(void)
{
    mdm_hangup();
    mdm_resp_reset();
    mdm_set_response_fn_state(mdm_response_code, 3); // NO CARRIER
}

static void mdm_carrier_lost(void)
{
    if (mdm_conn->state == mdm_state_on_hook)
        return;
    DBG("NET MDM carrier lost\n");
    // Remote FIN while DTE is in data mode: defer NO CARRIER until
    // mdm_std_read has drained net's buffered pbufs. net is already in
    // net_state_closing and will self-close on drain.
    if (mdm_conn->state == mdm_state_connected && !mdm_conn->in_command_mode)
    {
        mdm_conn->state = mdm_state_disconnecting;
        return;
    }
    mdm_finalize_carrier_lost();
}

bool mdm_conns_is_open(int desc)
{
    return mdm_conns[desc].is_open;
}

uint16_t mdm_conns_listen_port(int desc)
{
    return mdm_conns[desc].settings.listen_port;
}

uint8_t mdm_get_ring_count(void)
{
    return mdm_conn->ring_count;
}

static void mdm_ring(void)
{
    if (mdm_conn->state != mdm_state_on_hook)
        return;
    mdm_conn->state = mdm_state_ringing;
    mdm_conn->ring_count = 0;
    mdm_conn->ring_timer = get_absolute_time();
    mdm_conn->is_answering = false;
}

static void mdm_net_on_close(int desc)
{
    mdm_set_conn(desc);
    mdm_carrier_lost();
}

bool mdm_answer(void)
{
    if (mdm_conn->state != mdm_state_ringing)
        return false;
    mdm_conn->is_answering = true;
    if (!tel_accept(mdm_desc(), mdm_conn->settings.listen_port,
                    mdm_conn->settings.net_mode != 0, mdm_conn->settings.tty_type,
                    mdm_net_on_close))
    {
        // Call gone — answered elsewhere or remote hung up
        mdm_conn->state = mdm_state_on_hook;
        mdm_conn->in_command_mode = true;
        mdm_conn->ring_count = 0;
        mdm_resp_reset();
        mdm_set_response_fn_state(mdm_response_code, 3); // NO CARRIER
        return true;
    }
    mdm_conn->state = mdm_state_connected;
    mdm_conn->in_command_mode = false;
    mdm_conn->ring_count = 0;
    mdm_resp_reset();
    if (mdm_conn->settings.progress > 0)
        mdm_set_response_fn_state(mdm_response_code, 5); // CONNECT 1200
    else
        mdm_set_response_fn_state(mdm_response_code, 1); // CONNECT
    return true;
}

static bool mdm_net_on_accept(uint16_t port)
{
    // Only one modem takes the call; the rest stay on-hook.
    for (int i = 0; i < NET_MDM_DESCS; i++)
    {
        if (!mdm_conns[i].is_open)
            continue;
        if (mdm_conns[i].settings.listen_port != port)
            continue;
        if (mdm_conns[i].state != mdm_state_on_hook)
            continue;
        mdm_set_conn(i);
        mdm_ring();
        return true;
    }
    return false;
}

static void mdm_listen_update(void)
{
    uint16_t active = mdm_conn->active_listen_port;
    uint16_t wanted = mdm_conn->settings.listen_port;
    if (active == wanted)
        return;
    if (active > 0)
    {
        if (mdm_conn->state == mdm_state_ringing)
        {
            tel_reject(active);
            mdm_conn->state = mdm_state_on_hook;
            mdm_conn->ring_count = 0;
        }
        tel_listen_close(active);
        mdm_conn->active_listen_port = 0;
    }
    if (wanted == 0)
        return;
    if (wanted == com_tel_get_port())
    {
        DBG("NET MDM %d listen_port conflicts with console, reset to 0\n", mdm_desc());
        mdm_conn->settings.listen_port = 0;
        return;
    }
    if (!wfi_ready())
        return;
    if (tel_listen(wanted, mdm_net_on_accept))
    {
        mdm_conn->active_listen_port = wanted;
        DBG("NET MDM %d listening on port %u\n", mdm_desc(), wanted);
    }
}

bool mdm_set_listen_port(uint16_t port)
{
    if (port > 0 && port == com_tel_get_port())
        return false;
    mdm_conn->settings.listen_port = port;
    mdm_listen_update();
    return true;
}

void __in_flash("mdm_init") mdm_init(void)
{
    mdm_stop();
}

void mdm_task()
{
    for (int i = 0; i < NET_MDM_DESCS; i++)
    {
        if (!mdm_conns[i].is_open)
            continue;
        mdm_set_conn(i);
        if (mdm_conn->parse_active)
        {
            if (mdm_resp_busy())
                continue;
            if (!mdm_conn->parse_result)
            {
                mdm_conn->parse_active = false;
                mdm_set_response_fn_state(mdm_response_code, 4); // ERROR
            }
            else if (*mdm_conn->parse_str == 0)
            {
                mdm_conn->parse_active = false;
                if (mdm_conn->in_command_mode)
                    mdm_set_response_fn(mdm_response_code); // OK
            }
            else
            {
                mdm_conn->parse_result = cmd_parse(&mdm_conn->parse_str);
            }
        }
        mdm_listen_update();
        if (mdm_conn->state == mdm_state_ringing)
        {
            if (!tel_has_pending(mdm_conn->settings.listen_port))
            {
                // Call gone (answered elsewhere or remote hung up)
                mdm_conn->state = mdm_state_on_hook;
                mdm_conn->in_command_mode = true;
                mdm_conn->ring_count = 0;
                mdm_resp_reset();
                mdm_set_response_fn_state(mdm_response_code, 3); // NO CARRIER
            }
            else if (time_reached(mdm_conn->ring_timer) &&
                     !mdm_resp_busy())
            {
                mdm_conn->ring_count++;
                mdm_set_response_fn_state(mdm_response_code, 2); // RING
                mdm_conn->ring_timer = make_timeout_time_ms(6000);
                if (mdm_conn->settings.auto_answer > 0 &&
                    mdm_conn->ring_count >= mdm_conn->settings.auto_answer)
                {
                    mdm_answer();
                }
            }
        }
        if (mdm_conn->state == mdm_state_wait)
        {
            if (wfi_ready())
            {
                if (tel_open(mdm_desc(), mdm_conn->cmd_buf, mdm_conn->dial_port,
                             mdm_net_on_close))
                    mdm_conn->state = mdm_state_dialing;
                else
                {
                    DBG("NET MDM dial failed after wifi ready\n");
                    mdm_conn->state = mdm_state_on_hook;
                    mdm_conn->in_command_mode = true;
                    mdm_set_response_fn_state(mdm_response_code, 3); // NO CARRIER
                }
            }
            else if (!wfi_connecting())
            {
                DBG("NET MDM dial failed, wifi not connecting\n");
                mdm_conn->state = mdm_state_on_hook;
                mdm_conn->in_command_mode = true;
                mdm_set_response_fn_state(mdm_response_code, 3); // NO CARRIER
            }
        }
        if (mdm_conn->escape_count == MDM_ESCAPE_COUNT &&
            time_reached(mdm_conn->escape_guard))
        {
            mdm_conn->escape_count = 0;
            if (!mdm_conn->in_command_mode)
            {
                mdm_conn->cmd_buf_len = 0;
                mdm_conn->in_command_mode = true;
                mdm_resp_reset();
                mdm_set_response_fn(mdm_response_code); // OK
            }
        }
    }
}

static void mdm_conn_stop(mdm_conn_t *conn)
{
    mdm_conn = conn;
    if (conn->active_listen_port > 0)
    {
        tel_listen_close(conn->active_listen_port);
        conn->active_listen_port = 0;
    }
    tel_close((int)(conn - mdm_conns));
    conn->is_open = false;
    conn->cmd_buf_len = 0;
    conn->response_buf_head = 0;
    conn->response_buf_tail = 0;
    mdm_resp_reset();
    conn->parse_result = true;
    conn->state = mdm_state_on_hook;
    conn->in_command_mode = true;
    conn->parse_active = false;
    conn->escape_count = 0;
    conn->ring_count = 0;
    conn->is_answering = false;
}

void mdm_stop(void)
{
    for (int i = 0; i < NET_MDM_DESCS; i++)
        mdm_conn_stop(&mdm_conns[i]);
}

// Output stage for the response renderer: maps a canonical '\n' to the
// configured S3/S4 CR-LF (inserting CR before a lone LF, idempotent after an
// explicit '\r', honoring the high-bit disable) and writes into the active
// read. Returns false at the read's count so the render pauses.
static char *mdm_sink_buf;
static uint32_t mdm_sink_count;
static uint32_t mdm_sink_pos;

static bool mdm_sink(char ch)
{
    uint8_t cr = mdm_conn->settings.cr_char;
    uint8_t lf = mdm_conn->settings.lf_char;
    char out[2];
    int n = 0;
    if (ch == '\r')
    {
        if (!(cr & 0x80))
            out[n++] = (char)cr;
    }
    else if (ch == '\n')
    {
        if (!mdm_conn->resp_prev_cr && !(cr & 0x80))
            out[n++] = (char)cr;
        if (!(lf & 0x80))
            out[n++] = (char)lf;
    }
    else
        out[n++] = ch;
    if (mdm_sink_pos + (uint32_t)n > mdm_sink_count)
        return false;
    for (int i = 0; i < n; i++)
        mdm_sink_buf[mdm_sink_pos++] = out[i];
    mdm_conn->resp_prev_cr = (ch == '\r');
    return true;
}

bool mdm_std_handles(const char *filename)
{
    if (strncasecmp(filename, "AT", 2) != 0)
        return false;
    if (filename[2] == ':')
        return true;
    if (filename[2] >= '0' && filename[2] <= '9' && filename[3] == ':')
        return true;
    return false;
}

int mdm_std_open(const char *path, uint8_t flags, api_errno *err)
{
    (void)flags;
    if (!cyw_get_rf_enable())
    {
        *err = API_ENODEV;
        return -1;
    }
    int desc = -1;
    for (int i = 0; i < NET_MDM_DESCS; i++)
    {
        if (!mdm_conns[i].is_open)
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
    mdm_set_conn(desc);
    const char *filename = path;
    if (!strncasecmp(filename, "AT", 2))
    {
        filename += 2;
        if (*filename >= '0' && *filename <= '9')
        {
            mdm_conn->settings_slot = *filename - '0';
            filename++;
        }
        else
            mdm_conn->settings_slot = -1;
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
    if (mdm_conn->settings_slot >= 0)
        mdm_read_settings(&mdm_conn->settings);
    else
        mdm_factory_settings(&mdm_conn->settings);
    mdm_conn->is_open = true;
    mdm_listen_update();
    // Optionally process filename as AT command
    // after NVRAM read. e.g. AT0:&F
    if (filename[0])
    {
        mdm_conn->parse_active = true;
        mdm_conn->parse_result = true;
        snprintf(mdm_conn->cmd_buf, sizeof(mdm_conn->cmd_buf), "%s", filename);
        mdm_conn->parse_str = mdm_conn->cmd_buf;
    }
    return desc;
}

std_rw_result mdm_std_close(int desc, api_errno *err)
{
    if (desc < 0 || desc >= NET_MDM_DESCS || !mdm_conns[desc].is_open)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    mdm_conn_stop(&mdm_conns[desc]);
    return STD_OK;
}

std_rw_result mdm_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    if (desc < 0 || desc >= NET_MDM_DESCS || !mdm_conns[desc].is_open)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    mdm_set_conn(desc);
    uint32_t pos = 0;
    for (;;)
    {
        // Drain the raw character-echo ring first.
        if (!mdm_response_buf_empty())
        {
            while (pos < count && !mdm_response_buf_empty())
            {
                buf[pos++] = mdm_conn->response_buf[mdm_conn->response_buf_tail];
                mdm_conn->response_buf_tail = (mdm_conn->response_buf_tail + 1) % MDM_RESPONSE_BUF_SIZE;
            }
            if (pos >= count)
                break;
        }
        // Render the queued response, S3/S4-translated. Responders self-format
        // to MDM_RESPONSE_WIDTH and emit explicit newlines, so no word-wrap.
        if (mdm_resp_busy())
        {
            if (pos >= count)
                break;
            // Refill the chunk when drained; an empty chunk with a live state is
            // an async await (e.g. a scan still running) — resume on a later read.
            if (mdm_conn->resp_pos >= mdm_conn->resp_len)
            {
                mdm_conn->resp_buf[0] = 0;
                mdm_conn->resp_state = mdm_conn->resp_fn(mdm_conn->resp_buf,
                                                         MDM_RESPONSE_RENDER_SIZE,
                                                         mdm_conn->resp_state,
                                                         MDM_RESPONSE_WIDTH);
                mdm_conn->resp_len = strlen(mdm_conn->resp_buf);
                mdm_conn->resp_pos = 0;
                if (mdm_conn->resp_len == 0)
                {
                    if (mdm_conn->resp_state >= 0)
                        break; // nothing yet; resume later
                    continue;  // generator done with no output
                }
            }
            mdm_sink_buf = buf;
            mdm_sink_count = count;
            mdm_sink_pos = pos;
            while (mdm_conn->resp_pos < mdm_conn->resp_len)
            {
                if (!mdm_sink(mdm_conn->resp_buf[mdm_conn->resp_pos]))
                    break;
                mdm_conn->resp_pos++;
            }
            pos = mdm_sink_pos;
            if (mdm_conn->resp_pos < mdm_conn->resp_len)
                break; // read buffer full; resume mid-chunk on a later read
            continue;
        }
        // Read from the telephone connection in data mode.
        if (!mdm_conn->in_command_mode)
        {
            uint16_t got = tel_rx(mdm_desc(), &buf[pos], (uint16_t)(count - pos));
            pos += got;
            if (got == 0 && mdm_conn->state == mdm_state_disconnecting)
            {
                // Buffered RX drained after remote FIN; emit NO CARRIER now.
                mdm_finalize_carrier_lost();
                continue;
            }
        }
        break;
    }
    *bytes_read = pos;
    return STD_OK;
}

std_rw_result mdm_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    if (desc < 0 || desc >= NET_MDM_DESCS || !mdm_conns[desc].is_open)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    mdm_set_conn(desc);
    if (mdm_conn->parse_active)
    {
        *bytes_written = 0;
        return STD_OK;
    }
    if (mdm_conn->in_command_mode)
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
    if (mdm_conn->state != mdm_state_connected)
    {
        // DTE flow control: no transport (dial in progress, carrier draining).
        // Mirrors a real modem holding CTS low.
        *bytes_written = 0;
        return STD_OK;
    }
    uint16_t bw = count > UINT16_MAX ? UINT16_MAX : (uint16_t)count;
    bw = tel_tx(mdm_desc(), buf, bw);
    for (uint16_t i = 0; i < bw; i++)
        mdm_tx_escape_observer(buf[i]);
    *bytes_written = bw;
    return STD_OK;
}

#endif /* RP6502_RIA_W */
