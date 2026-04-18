/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W
#include "net/mdm.h"
mdm_settings_t *mdm_settings;
void mdm_task(void) {}
void mdm_stop(void) {}
void mdm_init(void) {}
void mdm_set_conn(int desc) { (void)desc; }
bool mdm_settings_persistent(void) { return false; }
bool mdm_std_handles(const char *) { return false; }
int mdm_std_open(const char *, uint8_t, api_errno *) { return -1; }
int mdm_std_close(int, api_errno *) { return -1; }
std_rw_result mdm_std_read(int, char *, uint32_t, uint32_t *, api_errno *) { return STD_ERROR; }
std_rw_result mdm_std_write(int, const char *, uint32_t, uint32_t *, api_errno *) { return STD_ERROR; }
void mdm_ring(void) {}
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
void mdm_listen_update(void) {}
#else

#include "net/cmd.h"
#include "net/mdm.h"
#include "net/net.h"
#include "net/tel.h"
#include "net/wfi.h"
#include "str/str.h"
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

typedef enum
{
    mdm_state_on_hook,
    mdm_state_ringing,
    mdm_state_wait,
    mdm_state_dialing,
    mdm_state_connected,
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
    char response_buf[MDM_RESPONSE_BUF_SIZE];
    size_t response_buf_head;
    size_t response_buf_tail;
    int (*response_fn)(char *, size_t, int);
    int response_state;
    bool is_parsing;
    const char *parse_str;
    bool parse_result;
    uint16_t dial_port;
    unsigned escape_count;
    absolute_time_t escape_last_char;
    absolute_time_t escape_guard;
    uint8_t ring_count;
    absolute_time_t ring_timer;
    uint16_t active_listen_port;
} mdm_conn_t;

static mdm_conn_t mdm_conns[NET_MDM_DESCS];
static mdm_conn_t *mdm_conn;
mdm_settings_t *mdm_settings;

static void mdm_net_on_close(int desc);
static bool mdm_net_on_accept(uint16_t port);

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
    mdm_settings = &mdm_conn->settings;
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

static inline size_t mdm_response_buf_count(void)
{
    if (mdm_conn->response_buf_head >= mdm_conn->response_buf_tail)
        return mdm_conn->response_buf_head - mdm_conn->response_buf_tail;
    else
        return MDM_RESPONSE_BUF_SIZE - mdm_conn->response_buf_tail + mdm_conn->response_buf_head;
}

void mdm_set_response_fn(int (*fn)(char *, size_t, int), int state)
{
    if (mdm_conn->response_state >= 0)
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
        mdm_conn->response_buf_head = mdm_conn->response_buf_tail = 0;
    }
    mdm_conn->response_fn = fn;
    mdm_conn->response_state = state;
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
    if (!(mdm_settings->cr_char & 0x80))
        mdm_response_append(mdm_settings->cr_char);
    if (!(mdm_settings->lf_char & 0x80))
        mdm_response_append(mdm_settings->lf_char);
}

static bool mdm_cmd_buf_is_at_command(void)
{
    return (mdm_conn->cmd_buf[0] == 'a' || mdm_conn->cmd_buf[0] == 'A') &&
           (mdm_conn->cmd_buf[1] == 't' || mdm_conn->cmd_buf[1] == 'T');
}

static int mdm_tx_command_mode(char ch)
{
    if (mdm_conn->response_state >= 0)
        return 0;
    if (ch == '\r' || (!(mdm_settings->cr_char & 0x80) && ch == mdm_settings->cr_char))
    {
        if (mdm_settings->echo)
            mdm_response_append_cr_lf();
        mdm_conn->cmd_buf[mdm_conn->cmd_buf_len] = 0;
        mdm_conn->cmd_buf_len = 0;
        if (mdm_cmd_buf_is_at_command())
        {
            mdm_conn->is_parsing = true;
            mdm_conn->parse_result = true;
            mdm_conn->parse_str = &mdm_conn->cmd_buf[2];
        }
    }
    else if (ch == 127 || (!(mdm_settings->bs_char & 0x80) && ch == mdm_settings->bs_char))
    {
        if (mdm_settings->echo)
        {
            mdm_response_append(mdm_settings->bs_char);
            mdm_response_append(' ');
            mdm_response_append(mdm_settings->bs_char);
        }
        if (mdm_conn->cmd_buf_len)
            mdm_conn->cmd_buf[--mdm_conn->cmd_buf_len] = 0;
    }
    else if (ch >= 32 && ch < 127)
    {
        if (mdm_settings->echo)
            mdm_response_append(ch);
        if (ch == '/' && mdm_conn->cmd_buf_len == 1)
        {
            if (mdm_settings->echo || (!mdm_settings->quiet && mdm_settings->verbose))
                mdm_response_append_cr_lf();
            mdm_conn->cmd_buf_len = 0;
            mdm_conn->is_parsing = true;
            if (mdm_cmd_buf_is_at_command())
            {

                mdm_conn->parse_result = true;
                mdm_conn->parse_str = &mdm_conn->cmd_buf[2];
            }
            else
                mdm_conn->parse_result = false; // immediate error
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
    if (mdm_settings->esc_char >= 128)
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
        if (ch != mdm_settings->esc_char)
            mdm_conn->escape_count = 0;
        else if (++mdm_conn->escape_count == MDM_ESCAPE_COUNT)
            mdm_conn->escape_guard = make_timeout_time_us(MDM_ESCAPE_GUARD_TIME_US);
    }
    mdm_conn->escape_last_char = get_absolute_time();
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
    unsigned x = mdm_settings->progress;
    if (x > 4)
        x = 4;
    bool suppress = false;
    if (mdm_settings->quiet == 1)
        suppress = true;
    // TODO quiet == 2 is different when answering
    else if (mdm_settings->quiet == 2 && state == 2)
        suppress = true;
    else if (!(x_mask[x] & (1u << state)))
        suppress = true;
    if (suppress)
        buf[0] = 0;
    else if (mdm_settings->verbose)
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
    for (; lfs_gets(mdm_phone_buf, sizeof(mdm_phone_buf), &lfs_volume, &lfs_file); index--)
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
                                     LFS_O_RDWR | LFS_O_CREAT,
                                     &lfs_file_config);
    if (lfsresult < 0)
        DBG("?Unable to lfs_file_opencfg %s for writing (%d)\n", settings_file, lfsresult);
    if (lfsresult >= 0)
        if ((lfsresult = lfs_file_truncate(&lfs_volume, &lfs_file, 0)) < 0)
            DBG("?Unable to lfs_file_truncate %s (%d)\n", settings_file, lfsresult);
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
    while (lfs_gets(line, sizeof(line), &lfs_volume, &lfs_file))
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
        tel_reject(mdm_settings->listen_port);
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
    mdm_conn->is_parsing = false;
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
        if (mdm_settings->progress > 0)
            mdm_set_response_fn(mdm_response_code, 5); // CONNECT 1200
        else
            mdm_set_response_fn(mdm_response_code, 1); // CONNECT
        mdm_conn->state = mdm_state_connected;
        mdm_conn->in_command_mode = false;
        tel_negotiate(mdm_desc());
        return true;
    }
    return false;
}

bool mdm_hangup(void)
{
    if (mdm_conn->state == mdm_state_ringing)
    {
        tel_reject(mdm_settings->listen_port);
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

void mdm_carrier_lost(void)
{
    if (mdm_conn->state != mdm_state_on_hook)
    {
        DBG("NET MDM carrier lost\n");
        mdm_hangup();
        mdm_set_response_fn(mdm_response_code, 3); // NO CARRIER
    }
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

void mdm_ring(void)
{
    if (mdm_conn->state != mdm_state_on_hook)
        return;
    mdm_conn->state = mdm_state_ringing;
    mdm_conn->ring_count = 0;
    mdm_conn->ring_timer = get_absolute_time();
}

bool mdm_answer(void)
{
    if (mdm_conn->state != mdm_state_ringing)
        return false;
    if (!tel_accept(mdm_desc(), mdm_settings->listen_port, mdm_net_on_close))
    {
        // Call gone — answered elsewhere or remote hung up
        mdm_conn->state = mdm_state_on_hook;
        mdm_conn->in_command_mode = true;
        mdm_conn->ring_count = 0;
        mdm_set_response_fn(mdm_response_code, 3); // NO CARRIER
        return true;
    }
    mdm_conn->state = mdm_state_connected;
    mdm_conn->in_command_mode = false;
    mdm_conn->ring_count = 0;
    if (mdm_settings->progress > 0)
        mdm_set_response_fn(mdm_response_code, 5); // CONNECT 1200
    else
        mdm_set_response_fn(mdm_response_code, 1); // CONNECT
    return true;
}

static void mdm_net_on_close(int desc)
{
    mdm_set_conn(desc);
    mdm_carrier_lost();
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
        if (!net_is_closed(i))
            continue;
        mdm_set_conn(i);
        mdm_ring();
        return true;
    }
    return false;
}

void mdm_listen_update(void)
{
    uint16_t old_port = mdm_conn->active_listen_port;
    uint16_t new_port = mdm_settings->listen_port;
    if (old_port == new_port)
        return;
    if (old_port > 0)
    {
        if (mdm_conn->state == mdm_state_ringing)
        {
            tel_reject(old_port);
            mdm_conn->state = mdm_state_on_hook;
            mdm_conn->ring_count = 0;
        }
        tel_listen_close(old_port);
        mdm_conn->active_listen_port = 0;
    }
    if (new_port > 0 && wfi_ready())
    {
        if (tel_listen(new_port, mdm_net_on_accept))
        {
            mdm_conn->active_listen_port = new_port;
            DBG("NET MDM %d listening on port %u\n", mdm_desc(), new_port);
        }
    }
}

void mdm_init(void)
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
        if (mdm_conn->is_parsing)
        {
            if (mdm_conn->response_state >= 0)
                continue;
            if (!mdm_conn->parse_result)
            {
                mdm_conn->is_parsing = false;
                mdm_set_response_fn(mdm_response_code, 4); // ERROR
            }
            else if (*mdm_conn->parse_str == 0)
            {
                mdm_conn->is_parsing = false;
                if (mdm_conn->in_command_mode)
                    mdm_set_response_fn(mdm_response_code, 0); // OK
            }
            else
            {
                mdm_conn->parse_result = cmd_parse(&mdm_conn->parse_str);
            }
        }
        if (mdm_conn->active_listen_port == 0 &&
            mdm_settings->listen_port > 0 && wfi_ready())
        {
            if (mdm_settings->listen_port == tel_get_port())
                mdm_settings->listen_port = 0;
            else if (tel_listen(mdm_settings->listen_port, mdm_net_on_accept))
            {
                mdm_conn->active_listen_port = mdm_settings->listen_port;
                DBG("NET MDM %d listening on port %u\n", i, mdm_settings->listen_port);
            }
        }
        if (mdm_conn->state == mdm_state_ringing)
        {
            if (!tel_has_pending(mdm_settings->listen_port))
            {
                // Call gone (answered elsewhere or remote hung up)
                mdm_conn->state = mdm_state_on_hook;
                mdm_conn->in_command_mode = true;
                mdm_conn->ring_count = 0;
                mdm_set_response_fn(mdm_response_code, 3); // NO CARRIER
            }
            else if (time_reached(mdm_conn->ring_timer) &&
                     mdm_conn->response_state < 0)
            {
                mdm_conn->ring_count++;
                mdm_set_response_fn(mdm_response_code, 2); // RING
                mdm_conn->ring_timer = make_timeout_time_ms(6000);
                if (mdm_settings->auto_answer > 0 &&
                    mdm_conn->ring_count >= mdm_settings->auto_answer)
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
                    mdm_set_response_fn(mdm_response_code, 3); // NO CARRIER
                }
            }
            else if (!wfi_connecting())
            {
                DBG("NET MDM dial failed, wifi not connecting\n");
                mdm_conn->state = mdm_state_on_hook;
                mdm_conn->in_command_mode = true;
                mdm_set_response_fn(mdm_response_code, 3); // NO CARRIER
            }
        }
        if (mdm_conn->escape_count == MDM_ESCAPE_COUNT &&
            time_reached(mdm_conn->escape_guard))
        {
            mdm_conn->escape_count = 0;
            if (!mdm_conn->in_command_mode)
            {
                mdm_conn->in_command_mode = true;
                mdm_conn->cmd_buf_len = 0;
                mdm_set_response_fn(mdm_response_code, 0); // OK
            }
        }
    }
}

static void mdm_conn_stop(mdm_conn_t *conn)
{
    mdm_conn = conn;
    mdm_settings = &conn->settings;
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
    conn->response_state = -1;
    conn->parse_result = true;
    conn->state = mdm_state_on_hook;
    conn->in_command_mode = true;
    conn->is_parsing = false;
    conn->escape_count = 0;
    conn->ring_count = 0;
}

void mdm_stop(void)
{
    for (int i = 0; i < NET_MDM_DESCS; i++)
        mdm_conn_stop(&mdm_conns[i]);
}

static void mdm_translate_newlines(void)
{
    size_t out = 0;
    for (size_t i = 0; i < mdm_conn->response_buf_head; i++)
    {
        uint8_t ch = mdm_conn->response_buf[i];
        bool translated = false;
        if (ch == '\r')
        {
            ch = mdm_settings->cr_char;
            translated = true;
        }
        else if (ch == '\n')
        {
            ch = mdm_settings->lf_char;
            translated = true;
        }
        if (!translated || !(ch & 0x80))
            mdm_conn->response_buf[out++] = ch;
    }
    mdm_conn->response_buf_head = out;
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
        mdm_read_settings(mdm_settings);
    else
        mdm_factory_settings(mdm_settings);
    if (mdm_settings->listen_port > 0 &&
        mdm_settings->listen_port == tel_get_port())
    {
        mdm_settings->listen_port = 0;
        DBG("NET MDM %d listen_port conflicts with console, reset to 0\n", desc);
    }
    mdm_conn->is_open = true;
    if (mdm_settings->listen_port > 0 && wfi_ready())
    {
        if (tel_listen(mdm_settings->listen_port, mdm_net_on_accept))
        {
            mdm_conn->active_listen_port = mdm_settings->listen_port;
            DBG("NET MDM %d listening on port %u\n", desc, mdm_settings->listen_port);
        }
    }
    // Optionally process filename as AT command
    // after NVRAM read. e.g. AT0:&F
    if (filename[0])
    {
        mdm_conn->is_parsing = true;
        mdm_conn->parse_result = true;
        snprintf(mdm_conn->cmd_buf, sizeof(mdm_conn->cmd_buf), "%s", filename);
        mdm_conn->parse_str = mdm_conn->cmd_buf;
    }
    return desc;
}

int mdm_std_close(int desc, api_errno *err)
{
    if (desc < 0 || desc >= NET_MDM_DESCS || !mdm_conns[desc].is_open)
    {
        *err = API_EBADF;
        return -1;
    }
    mdm_conn_stop(&mdm_conns[desc]);
    return 0;
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
    while (pos < count)
    {
        // Refill response buffer from generator if needed
        if (mdm_response_buf_empty() && mdm_conn->response_state >= 0)
        {
            mdm_conn->response_state = mdm_conn->response_fn(mdm_conn->response_buf, MDM_RESPONSE_BUF_SIZE, mdm_conn->response_state);
            mdm_conn->response_buf_head = strlen(mdm_conn->response_buf);
            mdm_conn->response_buf_tail = 0;
            mdm_translate_newlines();
        }
        if (!mdm_response_buf_empty())
        {
            // Drain response buffer
            buf[pos++] = mdm_conn->response_buf[mdm_conn->response_buf_tail];
            mdm_conn->response_buf_tail = (mdm_conn->response_buf_tail + 1) % MDM_RESPONSE_BUF_SIZE;
        }
        else if (!mdm_conn->in_command_mode)
        {
            // Read from telephone connection in data mode
            pos += tel_rx(mdm_desc(), &buf[pos], (uint16_t)(count - pos));
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
    if (desc < 0 || desc >= NET_MDM_DESCS || !mdm_conns[desc].is_open)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    mdm_set_conn(desc);
    if (mdm_conn->is_parsing)
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
    uint16_t bw = count;
    if (mdm_conn->state == mdm_state_connected)
        bw = tel_tx(mdm_desc(), buf, bw);
    for (uint16_t i = 0; i < bw; i++)
        mdm_tx_escape_observer(buf[i]);
    *bytes_written = bw;
    return STD_OK;
}

#endif /* RP6502_RIA_W */
