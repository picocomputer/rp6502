/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "str/rln.h"
#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_RLN)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

typedef enum
{
    ansi_state_C0,
    ansi_state_Fe,
    ansi_state_SS2,
    ansi_state_SS3,
    ansi_state_CSI,
    ansi_state_CSI_private,
} rln_ansi_state_t;

#define RLN_BUF_SIZE 256
#define RLN_HISTORY_SIZE 3
#define RLN_CSI_PARAM_MAX_LEN 16

// History storage
static char rln_newest_buf[RLN_BUF_SIZE];
static char rln_history_run[RLN_HISTORY_SIZE][RLN_BUF_SIZE];
static char rln_history_mon[RLN_HISTORY_SIZE][RLN_BUF_SIZE];
static uint8_t rln_history_head_mon;
static uint8_t rln_history_count_mon;

// Current history
static char (*rln_history)[RLN_BUF_SIZE];
static uint8_t rln_history_head;
static uint8_t rln_history_count;
static int8_t rln_history_pos;

// Input state
static char *rln_buf;
static rln_read_callback_t rln_callback;
static absolute_time_t rln_timer;
static uint8_t rln_buflen;
static uint8_t rln_bufpos;
static rln_ansi_state_t rln_ansi_state;
static uint16_t rln_csi_param[RLN_CSI_PARAM_MAX_LEN];
static uint8_t rln_csi_param_count;
static uint32_t rln_ctrl_bits;

// Programmatic state
static bool rln_programmatic_mode;
static uint32_t rln_programmatic_saved_timeout_ms;
static bool rln_programmatic_saved_enable_history;

// Configuration and exposed status
static bool rln_suppress_end_move;
static bool rln_suppress_newline;
static bool rln_enable_history;
static uint8_t rln_max_length;
static uint32_t rln_timeout_ms;
static uint8_t rln_end_char;
static bool rln_timed_out;
static uint8_t rln_cursor_pos;

static void rln_complete(void)
{
    rln_read_callback_t cc = rln_callback;
    rln_callback = NULL;
    if (rln_programmatic_mode)
    {
        rln_timeout_ms = rln_programmatic_saved_timeout_ms;
        rln_enable_history = rln_programmatic_saved_enable_history;
        rln_programmatic_mode = false;
    }
    cc(rln_timed_out, rln_timed_out ? NULL : rln_buf, rln_timed_out ? 0 : rln_buflen);
}

static void rln_set_buf(void)
{
    if (rln_history_pos < 0)
        rln_buf = rln_newest_buf;
    else
    {
        uint8_t idx = (rln_history_head - 1 - rln_history_pos + RLN_HISTORY_SIZE) % RLN_HISTORY_SIZE;
        rln_buf = rln_history[idx];
    }
}

static void rln_line_redraw(void)
{
    if (rln_bufpos)
        printf("\33[%dD", rln_bufpos);
    if (rln_buflen)
        printf("\33[%dP", rln_buflen);
    size_t len = strlen(rln_buf);
    for (size_t i = 0; i < len; i++)
        putchar(rln_buf[i]);
    rln_bufpos = len;
    rln_buflen = len;
}

static void rln_line_up(void)
{
    if (!rln_enable_history)
        return;
    if (rln_history_count == 0)
        return;
    if (rln_history_pos < 0)
    {
        rln_buf[rln_buflen] = 0;
        rln_history_pos = 0;
    }
    else if (rln_history_pos < rln_history_count - 1)
    {
        rln_buf[rln_buflen] = 0;
        rln_history_pos++;
    }
    else // at oldest
        return;
    rln_set_buf();
    rln_line_redraw();
}

static void rln_line_down(void)
{
    if (!rln_enable_history)
        return;
    if (rln_history_pos < 0)
        return;
    rln_buf[rln_buflen] = 0;
    rln_history_pos--;
    rln_set_buf();
    rln_line_redraw();
}

static void rln_history_add(void)
{
    if (!rln_enable_history)
        return;
    if (rln_buflen == 0)
        return;
    if (rln_history_count > 0)
    {
        uint8_t last = (rln_history_head - 1 + RLN_HISTORY_SIZE) % RLN_HISTORY_SIZE;
        if (strcmp(rln_history[last], rln_buf) == 0)
            return;
    }
    for (size_t i = 0; i < rln_buflen; i++)
        rln_history[rln_history_head][i] = rln_buf[i];
    rln_history[rln_history_head][rln_buflen] = 0;
    rln_history_head = (rln_history_head + 1) % RLN_HISTORY_SIZE;
    if (rln_history_count < RLN_HISTORY_SIZE)
        rln_history_count++;
}

static void rln_line_home(void)
{
    if (rln_bufpos)
        printf("\33[%dD", rln_bufpos);
    rln_bufpos = 0;
}

static void rln_line_end(void)
{
    if (rln_bufpos != rln_buflen)
        printf("\33[%dC", rln_buflen - rln_bufpos);
    rln_bufpos = rln_buflen;
}

static bool rln_is_word_delimiter(char ch)
{
    return ch == ' ' || ch == '/' || ch == '\\' || ch == '.' || ch == ':' || ch == '=';
}

static void rln_line_forward_word(void)
{
    int count = 0;
    if (rln_bufpos < rln_buflen)
        while (true)
        {
            count++;
            if (++rln_bufpos >= rln_buflen)
                break;
            if (rln_is_word_delimiter(rln_buf[rln_bufpos]) &&
                !rln_is_word_delimiter(rln_buf[rln_bufpos - 1]))
                break;
        }
    if (count)
        printf("\33[%dC", count);
}

static void rln_line_forward(void)
{
    uint16_t count = rln_csi_param[0];
    if (count < 1)
        count = 1;
    if (rln_csi_param_count > 1 && !!(rln_csi_param[1] - 1))
        return rln_line_forward_word();
    if (count > rln_buflen - rln_bufpos)
        count = rln_buflen - rln_bufpos;
    if (!count)
        return;
    rln_bufpos += count;
    printf("\33[%dC", count);
}

static void rln_line_forward_1(void)
{
    rln_csi_param_count = 1;
    rln_csi_param[0] = 1;
    rln_line_forward();
}

static void rln_line_backward_word(void)
{
    int count = 0;
    if (rln_bufpos)
        while (true)
        {
            count++;
            if (!--rln_bufpos)
                break;
            if (!rln_is_word_delimiter(rln_buf[rln_bufpos]) &&
                rln_is_word_delimiter(rln_buf[rln_bufpos - 1]))
                break;
        }
    if (count)
        printf("\33[%dD", count);
}

static void rln_line_backward(void)
{
    uint16_t count = rln_csi_param[0];
    if (count < 1)
        count = 1;
    if (rln_csi_param_count > 1 && !!(rln_csi_param[1] - 1))
        return rln_line_backward_word();
    if (count > rln_bufpos)
        count = rln_bufpos;
    if (!count)
        return;
    rln_bufpos -= count;
    printf("\33[%dD", count);
}

static void rln_line_backward_1(void)
{
    rln_csi_param_count = 1;
    rln_csi_param[0] = 1;
    rln_line_backward();
}

static void rln_line_delete(void)
{
    if (!rln_buflen || rln_bufpos == rln_buflen)
        return;
    printf("\33[P");
    rln_buflen--;
    for (size_t i = rln_bufpos; i < rln_buflen; i++)
        rln_buf[i] = rln_buf[i + 1];
}

static void rln_line_backspace(void)
{
    if (!rln_bufpos)
        return;
    printf("\b\33[P");
    rln_buflen--;
    for (size_t i = --rln_bufpos; i < rln_buflen; i++)
        rln_buf[i] = rln_buf[i + 1];
}

static void rln_line_insert(char ch)
{
    if (ch < 32 || rln_buflen + 1 >= rln_max_length)
        return;
    for (size_t i = rln_buflen; i > rln_bufpos; i--)
        rln_buf[i] = rln_buf[i - 1];
    rln_buflen++;
    rln_buf[rln_bufpos] = ch;
    for (size_t i = rln_bufpos; i < rln_buflen; i++)
        putchar(rln_buf[i]);
    rln_bufpos++;
    if (rln_buflen - rln_bufpos)
        printf("\33[%dD", rln_buflen - rln_bufpos);
}

static void rln_line_state_C0(char ch)
{
    if (rln_ctrl_bits & (1 << ch))
    {
        printf("\n");
        rln_buf[0] = ch;
        rln_buf[1] = 0;
        rln_buflen = 1;
        rln_complete();
    }
    else if (ch == '\r')
    {
        printf("\n");
        rln_buf[rln_buflen] = 0;
        rln_history_add();
        rln_complete();
    }
    else if (ch == '\33')
        rln_ansi_state = ansi_state_Fe;
    else if (ch == '\b' || ch == 127)
        rln_line_backspace();
    else if (ch == 1) // ctrl-a
        rln_line_home();
    else if (ch == 2) // ctrl-b
        rln_line_backward_1();
    else if (ch == 5) // ctrl-e
        rln_line_end();
    else if (ch == 6) // ctrl-f
        rln_line_forward_1();
    else
        rln_line_insert(ch);
}

static void rln_line_state_Fe(char ch)
{
    if (ch == '[')
    {
        rln_ansi_state = ansi_state_CSI;
        rln_csi_param_count = 0;
        rln_csi_param[0] = 0;
    }
    else if (ch == 'b' || ch == 2)
    {
        rln_ansi_state = ansi_state_C0;
        rln_line_backward_word();
    }
    else if (ch == 'f' || ch == 6)
    {
        rln_ansi_state = ansi_state_C0;
        rln_line_forward_word();
    }
    else if (ch == 'N')
        rln_ansi_state = ansi_state_SS2;
    else if (ch == 'O')
        rln_ansi_state = ansi_state_SS3;
    else
    {
        rln_ansi_state = ansi_state_C0;
        if (ch == 127)
            rln_line_delete();
    }
}

static void rln_line_state_SS2(char ch)
{
    (void)ch;
    rln_ansi_state = ansi_state_C0;
}

static void rln_line_state_SS3(char ch)
{
    rln_ansi_state = ansi_state_C0;
    if (ch == 'F')
        rln_line_end();
    else if (ch == 'H')
        rln_line_home();
}

static void rln_line_state_CSI(char ch)
{
    // Silently discard overflow parameters but still count to + 1.
    if (isdigit(ch))
    {
        if (rln_csi_param_count < RLN_CSI_PARAM_MAX_LEN)
        {
            rln_csi_param[rln_csi_param_count] *= 10;
            rln_csi_param[rln_csi_param_count] += ch - '0';
        }
        return;
    }
    if (ch == ';' || ch == ':')
    {
        if (++rln_csi_param_count < RLN_CSI_PARAM_MAX_LEN)
            rln_csi_param[rln_csi_param_count] = 0;
        else
            rln_csi_param_count = RLN_CSI_PARAM_MAX_LEN;
        return;
    }
    if (ch == '<' || ch == '=' || ch == '>' || ch == '?')
    {
        rln_ansi_state = ansi_state_CSI_private;
        return;
    }
    if (rln_ansi_state == ansi_state_CSI_private)
    {
        rln_ansi_state = ansi_state_C0;
        return;
    }
    rln_ansi_state = ansi_state_C0;
    if (++rln_csi_param_count > RLN_CSI_PARAM_MAX_LEN)
        rln_csi_param_count = RLN_CSI_PARAM_MAX_LEN;
    if (ch == 'A')
        rln_line_up();
    else if (ch == 'B')
        rln_line_down();
    else if (ch == 'C')
        rln_line_forward();
    else if (ch == 'D')
        rln_line_backward();
    else if (ch == 'F')
        rln_line_end();
    else if (ch == 'H')
        rln_line_home();
    else if (ch == 'b' || ch == 2)
        rln_line_backward_word();
    else if (ch == 'f' || ch == 6)
        rln_line_forward_word();
    else if (ch == '~')
        switch (rln_csi_param[0])
        {
        case 1:
        case 7:
            return rln_line_home();
        case 4:
        case 8:
            return rln_line_end();
        case 3:
            return rln_line_delete();
        }
}

static void rln_line_rx(uint8_t ch)
{
    if (ch == '\30')
        rln_ansi_state = ansi_state_C0;
    else
        switch (rln_ansi_state)
        {
        case ansi_state_C0:
            rln_line_state_C0(ch);
            break;
        case ansi_state_Fe:
            rln_line_state_Fe(ch);
            break;
        case ansi_state_SS2:
            rln_line_state_SS2(ch);
            break;
        case ansi_state_SS3:
            rln_line_state_SS3(ch);
            break;
        case ansi_state_CSI:
        case ansi_state_CSI_private:
            rln_line_state_CSI(ch);
            break;
        }
}

void rln_read_line(rln_read_callback_t callback)
{
    rln_timed_out = false;
    rln_buflen = 0;
    rln_bufpos = 0;
    rln_ansi_state = ansi_state_C0;
    rln_timer = make_timeout_time_ms(rln_timeout_ms);
    rln_callback = callback;
    rln_history_pos = -1;
    rln_buf = rln_newest_buf;
}

void rln_read_line_programmatic(rln_read_callback_t callback, uint32_t timeout_ms)
{
    assert(timeout_ms);
    rln_programmatic_saved_timeout_ms = rln_timeout_ms;
    rln_programmatic_saved_enable_history = rln_enable_history;
    rln_programmatic_mode = true;
    rln_timeout_ms = timeout_ms;
    rln_enable_history = false;
    rln_read_line(callback);
}

void rln_task(void)
{
    if (!rln_callback)
        return;
    while (rln_callback)
    {
        int ch = stdio_getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT)
            break;
        rln_timer = make_timeout_time_ms(rln_timeout_ms);
        rln_line_rx(ch);
    }
    if (rln_callback && rln_timeout_ms &&
        absolute_time_diff_us(get_absolute_time(), rln_timer) < 0)
    {
        rln_timed_out = true;
        rln_complete();
    }
}

void rln_init(void)
{
    rln_callback = NULL;
    rln_history = rln_history_mon;
    rln_suppress_end_move = false;
    rln_suppress_newline = false;
    rln_enable_history = true;
    rln_max_length = 254;
    rln_timeout_ms = 0;
    rln_programmatic_saved_timeout_ms = 0;
    rln_ctrl_bits = 0;
    rln_end_char = '\r';
    rln_timed_out = false;
    rln_cursor_pos = 0xFF;
}

void rln_run(void)
{
    rln_init();
    rln_history = rln_history_run;
    rln_enable_history = false;
    // Preserve history counters
    rln_history_head_mon = rln_history_head;
    rln_history_count_mon = rln_history_count;
    // Run with clean history
    memset(rln_history_run, 0, sizeof(rln_history_run));
    rln_history_head = 0;
    rln_history_count = 0;
}

void rln_stop(void)
{
    rln_init();
    // Restore history counters
    rln_history_head = rln_history_head_mon;
    rln_history_count = rln_history_count_mon;
}

void rln_break(void)
{
    rln_init();
}

/* Readline configuration getters/setters */

bool rln_get_suppress_end_move(void) { return rln_suppress_end_move; }
void rln_set_suppress_end_move(bool v) { rln_suppress_end_move = v; }

bool rln_get_suppress_newline(void) { return rln_suppress_newline; }
void rln_set_suppress_newline(bool v) { rln_suppress_newline = v; }

bool rln_get_enable_history(void) { return rln_enable_history; }
void rln_set_enable_history(bool v) { rln_enable_history = v; }

uint8_t rln_get_max_length(void) { return rln_max_length; }
void rln_set_max_length(uint8_t v) { rln_max_length = v; }

uint32_t rln_get_timeout(void) { return rln_timeout_ms; }
void rln_set_timeout(uint32_t v) { rln_timeout_ms = v; }

uint32_t rln_get_ctrl_bits(void) { return rln_ctrl_bits; }
void rln_set_ctrl_bits(uint32_t v) { rln_ctrl_bits = v; }

uint8_t rln_get_cursor_pos(void) { return rln_cursor_pos; }
void rln_set_cursor_pos(uint8_t v) { rln_cursor_pos = v; }

uint8_t rln_get_end_char(void) { return rln_end_char; }

bool rln_get_timed_out(void) { return rln_timed_out; }
