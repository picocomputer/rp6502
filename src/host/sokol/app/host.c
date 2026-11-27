/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The host's answers to the machine (host/host.h) that depend on the window,
 * given from what the window last told the machine (seam_machine.h), on the
 * machine's thread. A test bench gives its own.
 */

#include "host/host.h"
#include "host/sokol/app/seam_machine.h"
#include "host/sokol/app/seam.h"
#include "osal/os.h"
#include <stdlib.h>
#include <string.h>

/* Sokol says when the window is minimized and when it is back. The web
 * player's page has neither, so there it is never minimized. */
bool host_minimized(void)
{
    return machine_minimized();
}

/* The clipboard is the host's while a window is up, and only the window may
 * reach it. A copy goes to the window as it is made. A paste asks for the
 * text as its turn comes: the machine asks the window and the paste waits
 * (HOST_CLIPBOARD_LATER) until the window's answer has come. An answer not
 * used within a moment is no longer the clipboard as it is, and is asked for
 * again. A web page's clipboard cannot be asked for at all: its text comes
 * only in a paste event (SEAM_PASTE).
 *
 * With no window, a script or a headless run, the clipboard is one the
 * emulator keeps for itself, empty until something is put in it. */
#define HOST_CLIPBOARD_FRESH_NS 100000000ull
static char *host_clipboard_given; /* the machine's, until it asks again */
static char *host_clipboard;

const char *host_clipboard_get(void)
{
    if (!machine_has_window())
        return host_clipboard ? host_clipboard : "";
#ifdef __EMSCRIPTEN__
    return NULL;
#else
    uint64_t at;
    char *in = machine_clipboard_take(&at);
    if (in && os_mono_ns() - at < HOST_CLIPBOARD_FRESH_NS)
    {
        free(host_clipboard_given);
        host_clipboard_given = in;
        return in;
    }
    free(in);
    machine_clipboard_ask();
    return HOST_CLIPBOARD_LATER;
#endif
}

bool host_clipboard_set(const char *utf8)
{
    if (machine_has_window())
        return seam_text_to_window(SEAM_COPY, utf8);
    const size_t n = strlen(utf8) + 1;
    char *copy = malloc(n);
    if (!copy)
        return false;
    memcpy(copy, utf8, n);
    free(host_clipboard);
    host_clipboard = copy;
    return true;
}
