/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "osal/console.h"

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static DWORD con_saved_in, con_saved_out;
static bool con_raw_on;
static bool con_out_saved;
static bool con_ended;
static WCHAR con_high; /* the console delivers a surrogate pair as two records */
/* What did not fit the last read, served first on the next, so a read of one
 * byte delivers one byte. Six bytes is the most one conversion produces: an
 * unpaired high surrogate becomes U+FFFD, three bytes, ahead of a character of
 * up to three more. */
static char con_carry[6];
static size_t con_carry_len;
static WCHAR con_repeat_ch;
static WORD con_repeat_left;

static volatile LONG con_break;

static BOOL WINAPI con_ctrl(DWORD type);

static HANDLE con_in(void)
{
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    return (h && h != INVALID_HANDLE_VALUE) ? h : NULL;
}

static HANDLE con_out(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    return (h && h != INVALID_HANDLE_VALUE) ? h : NULL;
}

/* GetConsoleMode is what tells a console from anything else, because
 * GetFileType answers FILE_TYPE_CHAR for NUL and for a serial port as well.
 * Both handles are asked, because a terminal on input with a file on output is
 * a pipeline. */
bool os_console_is_terminal(void)
{
    DWORD mode;
    HANDLE i = con_in(), o = con_out();
    return i && o && GetConsoleMode(i, &mode) && GetConsoleMode(o, &mode);
}

bool os_console_stdin_is_terminal(void)
{
    DWORD mode;
    HANDLE i = con_in();
    return i && GetConsoleMode(i, &mode);
}

bool os_console_stderr_is_terminal(void)
{
    DWORD mode;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    return h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) &&
           os_console_is_terminal();
}

static void con_restore(void)
{
    if (!con_raw_on)
        return;
    con_raw_on = false;
    HANDLE i = con_in(), o = con_out();
    if (i)
        SetConsoleMode(i, con_saved_in);
    if (o && con_out_saved)
        SetConsoleMode(o, con_saved_out);
}

bool os_console_break_asked(void)
{
    return con_break != 0;
}

void os_console_break_ask(void)
{
    InterlockedCompareExchange(&con_break, 1, 0);
}

void os_console_break_exit(void)
{
    con_restore();
    /* 0xC000013A is STATUS_CONTROL_C_EXIT, the code a process ended by the
     * default Ctrl-Break handler exits with. */
    ExitProcess(0xC000013AU);
}

static BOOL WINAPI con_ctrl(DWORD type)
{
    if (type == CTRL_BREAK_EVENT || type == CTRL_C_EVENT)
    {
        /* The first event is only recorded, because this handler runs on a
         * thread of the console's own and the machine goes down on the main
         * thread. A second event falls through to the default handler, which
         * ends the process. */
        if (!InterlockedExchange(&con_break, 1))
            return TRUE;
    }
    /* CTRL_CLOSE_EVENT ends the process before atexit runs, so the terminal is
     * given back here. */
    con_restore();
    return FALSE;
}

void os_console_attach(void)
{
    SetConsoleCtrlHandler(con_ctrl, TRUE);
    HANDLE pre_out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE pre_err = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE pre_in = GetStdHandle(STD_INPUT_HANDLE);
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        if (!pre_out || pre_out == INVALID_HANDLE_VALUE)
            freopen("CONOUT$", "w", stdout);
        if (!pre_err || pre_err == INVALID_HANDLE_VALUE)
            freopen("CONOUT$", "w", stderr);
        if (!pre_in || pre_in == INVALID_HANDLE_VALUE)
            freopen("CONIN$", "r", stdin);
    }
    /* A launcher may start this process with one of the three closed, and the
     * CRT hands the next file opened the lowest free descriptor. Pointing each
     * closed one at NUL first keeps a later fopen from becoming the descriptor
     * that stdio reads and writes. */
    for (int fd = 0; fd < 3; fd++)
        if (_get_osfhandle(fd) == -1)
            if (freopen("NUL", fd ? "w" : "r", fd == 0   ? stdin
                                              : fd == 1 ? stdout
                                                        : stderr) == NULL)
                break;
    atexit(con_restore);
}

void os_console_raw(bool on)
{
    HANDLE i = con_in(), o = con_out();
    if (on == con_raw_on || !os_console_stdin_is_terminal())
        return;
    if (!on)
    {
        con_restore();
        return;
    }
    if (!GetConsoleMode(i, &con_saved_in))
        return;
    con_out_saved = o && GetConsoleMode(o, &con_saved_out);
    /* Clearing ENABLE_PROCESSED_INPUT is what hands Ctrl-C to the machine.
     * Ctrl-Break does not go through that flag and still raises its event, so
     * it remains the way out when the machine has stopped reading. */
    DWORD mode = con_saved_in;
    mode &= ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                     ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT | ENABLE_QUICK_EDIT_MODE);
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS;
    if (!SetConsoleMode(i, mode))
        return;
    if (con_out_saved)
        SetConsoleMode(o, con_saved_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    con_raw_on = true;
}

static size_t con_put_wide(WCHAR w, char *buf, size_t count)
{
    WCHAR pair[2];
    int n = 0;
    if (w >= 0xD800 && w <= 0xDBFF)
    {
        con_high = w;
        return 0;
    }
    if (con_high)
    {
        pair[n++] = con_high;
        con_high = 0;
    }
    pair[n++] = w;
    int got = WideCharToMultiByte(CP_UTF8, 0, pair, n, buf, (int)count, NULL, NULL);
    return got > 0 ? (size_t)got : 0;
}

static size_t con_serve(char *buf, size_t count)
{
    size_t out = 0;
    for (;;)
    {
        size_t n = con_carry_len < count - out ? con_carry_len : count - out;
        for (size_t i = 0; i < n; i++)
            buf[out + i] = con_carry[i];
        out += n;
        con_carry_len -= n;
        for (size_t i = 0; i < con_carry_len; i++)
            con_carry[i] = con_carry[i + n];
        if (con_carry_len || !con_repeat_left || out >= count)
            return out;
        con_repeat_left--;
        con_carry_len = con_put_wide(con_repeat_ch, con_carry, sizeof con_carry);
    }
}

static size_t con_read_console(HANDLE h, char *buf, size_t count)
{
    size_t out = con_serve(buf, count);
    DWORD queued = 0;
    if (!GetNumberOfConsoleInputEvents(h, &queued))
        return out;
    while (queued-- && out < count)
    {
        INPUT_RECORD rec;
        DWORD got = 0;
        /* Every record is read, key or not, because a record this loop skips
         * would otherwise stay at the head of the queue for ever and the first
         * window or focus event would wedge the console. */
        if (!ReadConsoleInputW(h, &rec, 1, &got) || !got)
            break;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown ||
            !rec.Event.KeyEvent.uChar.UnicodeChar)
            continue;
        con_repeat_ch = rec.Event.KeyEvent.uChar.UnicodeChar;
        con_repeat_left = rec.Event.KeyEvent.wRepeatCount;
        out += con_serve(buf + out, count - out);
    }
    return out;
}

size_t os_console_read(char *buf, size_t count)
{
    HANDLE h = con_in();
    if (con_ended || !count || !h)
        return 0;
    DWORD mode;
    if (GetConsoleMode(h, &mode))
        return con_read_console(h, buf, count);
    if (GetFileType(h) == FILE_TYPE_PIPE)
    {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
        {
            con_ended = true; /* the writing end has closed */
            return 0;
        }
        if (!avail)
            return 0;
        if (avail < count)
            count = avail;
    }
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)count, &got, NULL) || !got)
    {
        con_ended = true;
        return 0;
    }
    return got;
}

bool os_console_ended(void)
{
    return con_ended;
}

bool os_console_wait(uint64_t ns)
{
    HANDLE h = con_in();
    if (con_ended || !h)
        return true;
    DWORD ms = (DWORD)(ns / 1000000u);
    if (GetFileType(h) == FILE_TYPE_PIPE)
    {
        /* Waiting on a pipe handle does not report that data has arrived, so
         * this is the one path that has to poll and sleep. */
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) || avail)
            return true;
        Sleep(ms ? ms : 1);
        return false;
    }
    return WaitForSingleObject(h, ms ? ms : 1) == WAIT_OBJECT_0;
}
