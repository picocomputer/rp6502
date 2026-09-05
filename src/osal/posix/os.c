/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/os.h"
#include "core/str/oem.h"
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>

uint64_t os_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void os_sleep_ns(uint64_t ns)
{
    struct timespec ts = {
        .tv_sec = (time_t)(ns / 1000000000ull),
        .tv_nsec = (long)(ns % 1000000000ull),
    };
    nanosleep(&ts, NULL);
}

bool os_localtime(time_t t, struct tm *out)
{
    return localtime_r(&t, out) != NULL;
}

bool os_gmtime(time_t t, struct tm *out)
{
    return gmtime_r(&t, out) != NULL;
}

#if defined(__APPLE__)
size_t strftime_l(char *restrict, size_t, const char *restrict,
                  const struct tm *restrict, locale_t);
#endif

/* Host locale used only for strftime, so the rest of the process stays in the
 * C locale. NULL if the environment locale isn't installed (falls back to C). */
static locale_t g_locale;

void os_locale_reset(void)
{
    if (!g_locale)
        g_locale = newlocale(LC_ALL_MASK, "", (locale_t)0);
}

size_t os_strftime_local(char *buf, size_t max, const char *fmt, const struct tm *tm)
{
    return g_locale ? strftime_l(buf, max, fmt, tm, g_locale)
                    : strftime(buf, max, fmt, tm);
}

void os_tm_apply_zone(struct tm *tm, const struct tm *probe)
{
#if defined(__GLIBC__) || defined(__APPLE__) || defined(__EMSCRIPTEN__) || defined(__USE_MISC)
    tm->tm_gmtoff = probe->tm_gmtoff;
    tm->tm_zone = probe->tm_zone;
#else
    (void)tm, (void)probe;
#endif
}

char *os_config_dir(void)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    const char *tail = "/rp6502-emu";
    if (!base || !base[0])
    {
        base = getenv("HOME");
        tail = "/.config/rp6502-emu";
    }
    if (!base || !base[0])
        return NULL;
    /* An environment variable is as long as the environment made it. */
    char *dir = malloc(strlen(base) + strlen(tail) + 1);
    if (dir)
        sprintf(dir, "%s%s", base, tail);
    return dir;
}

void os_ensure_parent_dir(const char *filepath)
{
    char *tmp = strdup(filepath); /* walked in place, so it is ours */
    if (!tmp)
        return;
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
    {
        free(tmp);
        return;
    }
    *slash = 0;
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/')
        {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
    free(tmp);
}

void os_console_attach(void) {}

/* ---- the host's stdin, where it is a machine's console wire ---- */

static struct termios stdin_saved;
static bool stdin_raw_on;
static bool stdin_at_eof;

/* tcsetattr from a background job raises SIGTTOU and the kernel stops us
 * before a frame is ever drawn, so the signal is held off across the call
 * and the terminal is left alone unless this process is the foreground one. */
static void stdin_apply(const struct termios *t)
{
    if (tcgetpgrp(STDIN_FILENO) != getpgrp())
        return;
    void (*was)(int) = signal(SIGTTOU, SIG_IGN);
    tcsetattr(STDIN_FILENO, TCSADRAIN, t);
    signal(SIGTTOU, was);
}

static void stdin_restore(void)
{
    if (!stdin_raw_on)
        return;
    stdin_raw_on = false;
    stdin_apply(&stdin_saved);
}

/* Ctrl-\ is the one key raw mode holds back from the machine, so it is the
 * way out of a program that has stopped listening. The first press asks; the
 * loop that owns the machine sees it and takes the machine down properly.
 * The second is for when nothing is reading the ask any more. */
static volatile sig_atomic_t stdin_break_asked;

bool os_break_asked(void)
{
    return stdin_break_asked != 0;
}

/* A signal that ends the process, and the one that resumes it: a terminal
 * left raw outlives the emulator, and a shell restores its own settings over
 * ours when a stopped job comes back. */
static void stdin_signal(int sig)
{
    if (sig == SIGCONT)
    {
        if (stdin_raw_on)
        {
            stdin_raw_on = false;
            os_stdin_raw(true);
        }
        return;
    }
    if (sig == SIGQUIT && !stdin_break_asked)
    {
        stdin_break_asked = 1;
        return;
    }
    stdin_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

bool os_stdin_is_terminal(void)
{
    return isatty(STDIN_FILENO) == 1;
}

bool os_stderr_is_terminal(void)
{
    return isatty(STDERR_FILENO) == 1;
}

void os_stdin_raw(bool on)
{
    if (on == stdin_raw_on || !os_stdin_is_terminal())
        return;
    if (!on)
    {
        stdin_restore();
        return;
    }
    if (tcgetattr(STDIN_FILENO, &stdin_saved))
        return;
    struct termios raw = stdin_saved;
    /* Not cfmakeraw: OPOST stays on, or every '\n' the emulator itself
     * prints would staircase down the screen. */
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ECHONL | IEXTEN);
    raw.c_iflag &= ~(tcflag_t)(ICRNL | INLCR | IGNCR | IXON | ISTRIP | INPCK | BRKINT);
    /* ISIG stays, but only for Ctrl-\. Ctrl-C and Ctrl-Z are the machine's
     * bytes now, and Ctrl-\ is what is left when the machine has stopped
     * answering them. */
    raw.c_cc[VINTR] = _POSIX_VDISABLE;
    raw.c_cc[VSUSP] = _POSIX_VDISABLE;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    stdin_raw_on = true;
    stdin_apply(&raw);
    static bool hooked;
    if (!hooked)
    {
        hooked = true;
        atexit(stdin_restore);
        static const int sigs[] = {SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGABRT,
                                   SIGFPE, SIGBUS, SIGSEGV, SIGTERM, SIGCONT};
        for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++)
            signal(sigs[i], stdin_signal);
    }
}

size_t os_stdin_read(char *buf, size_t count)
{
    if (stdin_at_eof || !count)
        return 0;
    struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
    if (poll(&p, 1, 0) <= 0)
        return 0;
    ssize_t n = read(STDIN_FILENO, buf, count);
    if (n > 0)
        return (size_t)n;
    /* A ready descriptor answering nothing is the far end gone. EAGAIN can
     * still happen on a terminal several readers share. */
    if (n == 0 || (errno != EAGAIN && errno != EINTR))
        stdin_at_eof = true;
    return 0;
}

bool os_stdin_ended(void)
{
    return stdin_at_eof;
}

bool os_stdin_wait(uint64_t ns)
{
    if (stdin_at_eof)
        return true;
    struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
    int ms = (int)(ns / 1000000u);
    return poll(&p, 1, ms > 0 ? ms : 1) > 0;
}

/* POSIX (and Emscripten) argv arrives as UTF-8. */
bool os_argv_to_oem(const char *arg, char *dst, size_t dstsz)
{
    return oem_from_utf8(arg, dst, dstsz) < dstsz;
}
