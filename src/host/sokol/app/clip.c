/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The clipboard (host/host.h), on the machine's thread. With a window up,
 * it is the desktop's or the web page's, which only the window may reach: a
 * copy goes to the window as it is made (EVQ_COPY), and a paste key's text
 * comes from the window as the key goes down (input.c), so the machine
 * never asks for it. With no window, a script or a headless run, it is one
 * the emulator keeps for itself, empty until something is put in it. A
 * test bench gives its own. */

#include "host/host.h"
#include "host/sokol/app/evq_machine.h"
#include "host/sokol/app/evq.h"
#include <stdlib.h>
#include <string.h>

static char *clip_kept;

const char *host_clipboard_get(void)
{
    return clip_kept && !machine_has_window() ? clip_kept : "";
}

void host_clipboard_set(const char *utf8)
{
    if (machine_has_window())
    {
        evq_text_to_window(EVQ_COPY, utf8);
        return;
    }
    const size_t n = strlen(utf8) + 1;
    char *copy = malloc(n);
    if (!copy)
        return;
    memcpy(copy, utf8, n);
    free(clip_kept);
    clip_kept = copy;
}
