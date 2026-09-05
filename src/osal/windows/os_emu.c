/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/os.h"
#include "core/str/oem.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

void os_console_attach(void)
{
    HANDLE pre_out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE pre_err = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE pre_in = GetStdHandle(STD_INPUT_HANDLE);
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    if (!pre_out || pre_out == INVALID_HANDLE_VALUE)
        freopen("CONOUT$", "w", stdout);
    if (!pre_err || pre_err == INVALID_HANDLE_VALUE)
        freopen("CONOUT$", "w", stderr);
    if (!pre_in || pre_in == INVALID_HANDLE_VALUE)
        freopen("CONIN$", "r", stdin);
}

/* ---- the host's stdin, where it is a machine's console wire ---- */

static DWORD stdin_saved_in, stdin_saved_out;
static bool stdin_raw_on;
static bool stdin_at_eof;
static WCHAR stdin_high; /* a surrogate whose pair is in the next record */

static HANDLE stdin_handle(void)
{
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    return (h && h != INVALID_HANDLE_VALUE) ? h : NULL;
}

/* A console answers GetConsoleMode; GetFileType alone would say FILE_TYPE_CHAR
 * for NUL and for a serial port too. */
bool os_stdin_is_terminal(void)
{
    DWORD mode;
    HANDLE h = stdin_handle();
    return h && GetConsoleMode(h, &mode);
}

bool os_stderr_is_terminal(void)
{
    DWORD mode;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    return h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
}

static void stdin_restore(void)
{
    if (!stdin_raw_on)
        return;
    stdin_raw_on = false;
    HANDLE h = stdin_handle();
    if (h)
        SetConsoleMode(h, stdin_saved_in);
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
    if (o && o != INVALID_HANDLE_VALUE)
        SetConsoleMode(o, stdin_saved_out);
}

static BOOL WINAPI stdin_ctrl(DWORD type)
{
    (void)type;
    stdin_restore(); /* CTRL_CLOSE_EVENT gives us only moments */
    return FALSE;    /* and the default handler still ends the process */
}

void os_stdin_raw(bool on)
{
    HANDLE h = stdin_handle();
    if (on == stdin_raw_on || !h || !os_stdin_is_terminal())
        return;
    if (!on)
    {
        stdin_restore();
        return;
    }
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(h, &stdin_saved_in))
        return;
    if (!o || o == INVALID_HANDLE_VALUE || !GetConsoleMode(o, &stdin_saved_out))
        return;
    /* Processed input off is what hands Ctrl-C to the machine. Ctrl-Break is
     * unaffected by it and still raises CTRL_BREAK_EVENT, which is the one
     * way out when the machine has stopped answering. */
    DWORD in = stdin_saved_in;
    in &= ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                   ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT | ENABLE_QUICK_EDIT_MODE);
    in |= ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS;
    if (!SetConsoleMode(h, in))
        return;
    /* The same console has to render what the machine draws. */
    SetConsoleMode(o, stdin_saved_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    stdin_raw_on = true;
    static bool hooked;
    if (!hooked)
    {
        hooked = true;
        atexit(stdin_restore);
        SetConsoleCtrlHandler(stdin_ctrl, TRUE);
    }
}

/* One console record's character, appended as UTF-8. A surrogate waits for
 * its pair, which arrives as the record after it. */
static size_t stdin_put_wide(WCHAR w, char *buf, size_t count)
{
    WCHAR pair[2];
    int n = 0;
    if (w >= 0xD800 && w <= 0xDBFF)
    {
        stdin_high = w;
        return 0;
    }
    if (stdin_high)
    {
        pair[n++] = stdin_high;
        stdin_high = 0;
    }
    pair[n++] = w;
    int got = WideCharToMultiByte(CP_UTF8, 0, pair, n, buf, (int)count, NULL, NULL);
    return got > 0 ? (size_t)got : 0;
}

static size_t stdin_read_console(HANDLE h, char *buf, size_t count)
{
    DWORD queued = 0;
    if (!GetNumberOfConsoleInputEvents(h, &queued) || !queued)
        return 0;
    size_t out = 0;
    while (queued-- && out + 4 <= count)
    {
        INPUT_RECORD rec;
        DWORD got = 0;
        /* Read every record, key or not: peeking past one this loop does not
         * want would leave it at the head of the queue for ever. */
        if (!ReadConsoleInputW(h, &rec, 1, &got) || !got)
            break;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown ||
            !rec.Event.KeyEvent.uChar.UnicodeChar)
            continue;
        for (WORD i = 0; i < rec.Event.KeyEvent.wRepeatCount && out + 4 <= count; i++)
            out += stdin_put_wide(rec.Event.KeyEvent.uChar.UnicodeChar, buf + out, count - out);
    }
    return out;
}

size_t os_stdin_read(char *buf, size_t count)
{
    HANDLE h = stdin_handle();
    if (stdin_at_eof || !count || !h)
        return 0;
    if (os_stdin_is_terminal())
        return stdin_read_console(h, buf, count);
    if (GetFileType(h) == FILE_TYPE_PIPE)
    {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
        {
            stdin_at_eof = true; /* the writer is gone */
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
        stdin_at_eof = true;
        return 0;
    }
    return got;
}

bool os_stdin_ended(void)
{
    return stdin_at_eof;
}

bool os_stdin_wait(uint64_t ns)
{
    HANDLE h = stdin_handle();
    if (stdin_at_eof || !h)
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

/* The ANSI main()'s argv is in the process ACP, not UTF-8. */
bool os_argv_to_oem(const char *arg, char *dst, size_t dstsz)
{
    int n = MultiByteToWideChar(CP_ACP, 0, arg, -1, NULL, 0); /* asks its own size */
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (!w || !MultiByteToWideChar(CP_ACP, 0, arg, -1, w, n))
    {
        free(w);
        return false;
    }
    bool ok = wcslen(w) < dstsz; /* one OEM byte per UTF-16 unit */
    if (ok)
        oem_from_wide((const uint16_t *)w, dst, dstsz);
    free(w);
    return ok;
}
