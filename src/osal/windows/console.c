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
static bool con_out_saved; /* stdout was a console when raw mode went on */
static bool con_ended;
static WCHAR con_high; /* a surrogate whose pair is in the next record */
/* What did not fit the last read, served first on the next, so a read of one
 * byte delivers one byte: the UTF-8 of one conversion (an ill-formed
 * surrogate pair converts to two characters), and the repeats of a key whose
 * record is already consumed. */
static char con_carry[6];
static size_t con_carry_len;
static WCHAR con_repeat_ch;
static WORD con_repeat_left;

/* Which ask arrived. There is no signal to re-raise here, so leaving is the
 * console's own default, which is what a Ctrl-Break normally does. */
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

/* A console answers GetConsoleMode; GetFileType alone would say
 * FILE_TYPE_CHAR for NUL and for a serial port too. Both ways, because a
 * terminal on input with a file on output is a pipeline. */
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
    /* The console's own answer for an interrupted run, which is what a
     * default Ctrl-Break handler produces. */
    ExitProcess(0xC000013AU);
}

static BOOL WINAPI con_ctrl(DWORD type)
{
    if (type == CTRL_BREAK_EVENT || type == CTRL_C_EVENT)
    {
        if (!InterlockedExchange(&con_break, 1))
            return TRUE; /* asked; the machine goes down on the main thread */
    }
    con_restore(); /* CTRL_CLOSE_EVENT gives us only moments */
    return FALSE;  /* and the default handler still ends the process */
}

void os_console_attach(void)
{
    /* Before the console is even found: Ctrl-Break is a console's own way of
     * asking, and every run on one may need answering. */
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
    /* A descriptor the launcher closed is one the machine's next open would
     * land on, and fd 0 in particular would then be read by the console. */
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
    /* Only a console has an output mode to save; a file on stdout has none
     * and is left alone below. */
    con_out_saved = o && GetConsoleMode(o, &con_saved_out);
    /* Processed input off is what hands Ctrl-C to the machine. Ctrl-Break is
     * unaffected by it and still raises its event, which is the one way out
     * when the machine has stopped answering. */
    DWORD mode = con_saved_in;
    mode &= ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                     ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT | ENABLE_QUICK_EDIT_MODE);
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS;
    if (!SetConsoleMode(i, mode))
        return;
    /* The same console has to render what the machine draws. */
    if (con_out_saved)
        SetConsoleMode(o, con_saved_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    con_raw_on = true;
}

/* One record's character, appended as UTF-8. A surrogate waits for its pair,
 * which arrives as the record after it. */
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

/* What fits of the carry, then of the repeats still owed. */
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
        /* Every record, key or not: peeking past one this loop does not want
         * would leave it at the head of the queue for ever, and a focus
         * event would wedge the console after the first Alt-Tab. */
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
            con_ended = true; /* the writer is gone */
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
        /* A pipe is not waitable, so this is the one place that sleeps. */
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) || avail)
            return true;
        Sleep(ms ? ms : 1);
        return false;
    }
    return WaitForSingleObject(h, ms ? ms : 1) == WAIT_OBJECT_0;
}
