/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "osal/console.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

/* The terminal as it was found, and the terminal as we want it. Two copies,
 * because a resumed job has to be put back into raw mode from the second and
 * never re-read the first: by then the first would be the raw one. */
static struct termios con_saved, con_raw;
static bool con_raw_on;
static bool con_ended;

/* Which signal asked, so leaving can be that signal rather than a number that
 * merely resembles one. A sig_atomic_t because a handler writes it. */
static volatile sig_atomic_t con_break_sig;

static bool con_same_file(int a, int b)
{
    struct stat sa, sb;
    if (fstat(a, &sa) || fstat(b, &sb))
        return false;
    if (!S_ISCHR(sa.st_mode) || !S_ISCHR(sb.st_mode))
        return false;
    return sa.st_rdev == sb.st_rdev;
}

/* One terminal both ways. A terminal on stdin with a file on stdout is a
 * pipeline, and a program's output belongs in the file rather than the
 * machine's screen. */
bool os_console_is_terminal(void)
{
    return isatty(STDIN_FILENO) == 1 && isatty(STDOUT_FILENO) == 1 &&
           con_same_file(STDIN_FILENO, STDOUT_FILENO);
}

bool os_console_stderr_is_terminal(void)
{
    return isatty(STDERR_FILENO) == 1 && con_same_file(STDERR_FILENO, STDOUT_FILENO);
}

/* tcsetattr from a background job raises SIGTTOU and the kernel stops us
 * before a frame is ever drawn, so the signal is held off across the call and
 * the terminal is left alone unless this process is the foreground one. */
static void con_apply(const struct termios *t)
{
    if (tcgetpgrp(STDIN_FILENO) != getpgrp())
        return;
    void (*was)(int) = signal(SIGTTOU, SIG_IGN);
    tcsetattr(STDIN_FILENO, TCSADRAIN, t);
    signal(SIGTTOU, was);
}

static void con_restore(void)
{
    if (!con_raw_on)
        return;
    con_raw_on = false;
    con_apply(&con_saved);
}

bool os_console_break_asked(void)
{
    return con_break_sig != 0;
}

void os_console_break_ask(void)
{
    if (!con_break_sig)
        con_break_sig = SIGHUP; /* nobody sent one; the far end simply left */
}

void os_console_break_exit(void)
{
    int sig = (int)con_break_sig;
    con_restore();
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig); /* a signal the default ignores still has to end this */
}

/* A signal that ends the process, and the one that resumes it. The first ask
 * is recorded and answered by whoever owns the machine, so the teardown runs;
 * a second is the hard way out, for a machine too wedged to reach it. */
static void con_signal(int sig)
{
    if (sig == SIGCONT)
    {
        if (con_raw_on)
            con_apply(&con_raw); /* the shell put its own modes back */
        return;
    }
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE ||
        sig == SIGABRT)
    {
        con_restore(); /* the program is broken; let it be broken loudly */
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    if (!con_break_sig)
    {
        con_break_sig = sig;
        return;
    }
    con_restore();
    _exit(128 + sig);
}

/* Never over SIG_IGN: a signal ignored on entry was ignored by whoever
 * started us, and nohup is the everyday case. */
static void con_hook(int sig)
{
    if (signal(sig, con_signal) == SIG_IGN)
        signal(sig, SIG_IGN);
}

void os_console_attach(void)
{
    /* Before anything can open a file: a descriptor the launcher closed is
     * one the machine's next open would land on, and fd 0 in particular
     * would then be read by the console. */
    for (int fd = 0; fd < 3; fd++)
        if (fcntl(fd, F_GETFD) == -1 && errno == EBADF)
            if (open("/dev/null", O_RDWR) != fd)
                break; /* they fill in order; out of order means give up */
    /* A write to a pipe whose reader has gone answers EPIPE instead of
     * killing us mid-frame, so the machine can be put away first. */
    signal(SIGPIPE, SIG_IGN);
    static const int sigs[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM,
                               SIGILL, SIGABRT, SIGFPE, SIGBUS, SIGSEGV};
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++)
        con_hook(sigs[i]);
    signal(SIGCONT, con_signal);
    atexit(con_restore);
}

void os_console_raw(bool on)
{
    if (on == con_raw_on || !os_console_is_terminal())
        return;
    if (!on)
    {
        con_restore();
        return;
    }
    if (tcgetattr(STDIN_FILENO, &con_saved))
        return;
    con_raw = con_saved;
    /* Not cfmakeraw: OPOST stays on, or every '\n' the emulator itself
     * prints would staircase down the screen. */
    con_raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ECHONL | IEXTEN);
    con_raw.c_iflag &= ~(tcflag_t)(ICRNL | INLCR | IGNCR | IXON | ISTRIP | INPCK | BRKINT);
    /* ISIG stays, but only for Ctrl-\. Ctrl-C and Ctrl-Z are the machine's
     * bytes now, and Ctrl-\ is what is left when the machine has stopped
     * answering them. VDSUSP is the same key on the BSDs and is gated by
     * ISIG rather than IEXTEN, so it has to go by name. */
    con_raw.c_cc[VINTR] = _POSIX_VDISABLE;
    con_raw.c_cc[VSUSP] = _POSIX_VDISABLE;
#ifdef VDSUSP
    con_raw.c_cc[VDSUSP] = _POSIX_VDISABLE;
#endif
    con_raw.c_cc[VMIN] = 1;
    con_raw.c_cc[VTIME] = 0;
    con_raw_on = true;
    con_apply(&con_raw);
}

size_t os_console_read(char *buf, size_t count)
{
    if (con_ended || !count)
        return 0;
    /* A read from a background process group raises SIGTTIN and the kernel
     * stops the job, which a machine that never asked for input should not
     * suffer for. */
    if (isatty(STDIN_FILENO) == 1 && tcgetpgrp(STDIN_FILENO) != getpgrp())
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
        con_ended = true;
    return 0;
}

bool os_console_ended(void)
{
    return con_ended;
}

bool os_console_wait(uint64_t ns)
{
    if (con_ended)
        return true;
    struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
    int ms = (int)(ns / 1000000u);
    return poll(&p, 1, ms > 0 ? ms : 1) > 0;
}
