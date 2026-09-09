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

/* Two copies of the terminal settings. con_saved is how the terminal was
 * found and is what exit restores. con_raw is what SIGCONT reapplies, because
 * the shell puts its own settings back while this job is stopped. */
static struct termios con_saved, con_raw;
static bool con_raw_on;
static bool con_ended;

/* Which signal asked to end the process, so the exit can be that same signal.
 * A volatile sig_atomic_t because a signal handler writes it. */
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

/* A terminal on stdin with a file on stdout is a pipeline, so the machine
 * takes the console only when both ends are the same terminal. */
bool os_console_is_terminal(void)
{
    return isatty(STDIN_FILENO) == 1 && isatty(STDOUT_FILENO) == 1 &&
           con_same_file(STDIN_FILENO, STDOUT_FILENO);
}

bool os_console_stdin_is_terminal(void)
{
    return isatty(STDIN_FILENO) == 1;
}

bool os_console_stderr_is_terminal(void)
{
    return isatty(STDERR_FILENO) == 1 && con_same_file(STDERR_FILENO, STDOUT_FILENO);
}

/* tcsetattr from a background job raises SIGTTOU, which stops the process, so
 * the terminal is touched only from the foreground process group and SIGTTOU
 * is held off across the call anyway. */
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
        con_break_sig = SIGHUP; /* no signal arrived; the far end simply left */
}

void os_console_break_exit(void)
{
    int sig = (int)con_break_sig;
    con_restore();
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

/* The first signal asking this process to end is only recorded, so that the
 * machine's own teardown runs and the terminal is put back. A second one
 * exits immediately, for a machine too wedged to reach the teardown. */
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
        con_restore(); /* put the terminal back, then take the default action */
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

/* A signal already ignored on entry was ignored by whatever started this
 * process, nohup being the everyday case, so leave it ignored. */
static void con_hook(int sig)
{
    if (signal(sig, con_signal) == SIG_IGN)
        signal(sig, SIG_IGN);
}

void os_console_attach(void)
{
    /* An open before this would land on any of 0, 1 and 2 the launcher left
     * closed, and the console would then read the file that got fd 0. */
    for (int fd = 0; fd < 3; fd++)
        if (fcntl(fd, F_GETFD) == -1 && errno == EBADF)
            if (open("/dev/null", O_RDWR) != fd)
                break; /* open fills the lowest free fd, so out of order means give up */
    /* A write to a pipe whose reader has gone answers EPIPE instead of ending
     * the process mid-frame, so the machine can be put away first. */
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
    if (on == con_raw_on || !os_console_stdin_is_terminal())
        return;
    if (!on)
    {
        con_restore();
        return;
    }
    if (tcgetattr(STDIN_FILENO, &con_saved))
        return;
    con_raw = con_saved;
    /* Not cfmakeraw, because OPOST has to stay on: without it every '\n' the
     * emulator prints would staircase down the screen. */
    con_raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ECHONL | IEXTEN);
    con_raw.c_iflag &= ~(tcflag_t)(ICRNL | INLCR | IGNCR | IXON | ISTRIP | INPCK | BRKINT);
    /* ISIG stays on for Ctrl-\, which is the way out when the machine has
     * stopped answering, while Ctrl-C and Ctrl-Z become the machine's own
     * bytes. */
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
    /* A read from a background process group raises SIGTTIN, which stops the
     * job, so a background machine reads nothing instead. */
    if (isatty(STDIN_FILENO) == 1 && tcgetpgrp(STDIN_FILENO) != getpgrp())
        return 0;
    struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
    if (poll(&p, 1, 0) <= 0)
        return 0;
    ssize_t n = read(STDIN_FILENO, buf, count);
    if (n > 0)
        return (size_t)n;
    /* A ready descriptor that reads nothing means the far end has gone.
     * EAGAIN can still happen on a terminal that several readers share. */
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
