/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/random.h"
#include "host/host.h"
#include "tb_seed.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

uint32_t host_seed(void)
{
    return 0x6502C0DE;
}

const char *host_save_dir(void)
{
    return NULL;
}

bool host_minimized(void)
{
    return false;
}

/* The test bench's host is a Mac when a test says so (tb_seed.h). */
bool tb_mac_keys;

bool host_mac_keys(void)
{
    return tb_mac_keys;
}

/* The test bench's host has a clipboard, empty until something is put in
 * it, as a desktop's is. */
static char *tb_clipboard;

const char *host_clipboard_get(void)
{
    return tb_clipboard ? tb_clipboard : "";
}

bool host_clipboard_set(const char *utf8)
{
    const size_t n = strlen(utf8) + 1;
    free(tb_clipboard);
    tb_clipboard = malloc(n);
    memcpy(tb_clipboard, utf8, n);
    return true;
}
