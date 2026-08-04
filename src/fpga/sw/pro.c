/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * argv, which on this machine is one string: the name of the .rp6502 the
 * user picked from the Pocket's menu. The core is handed the image
 * already staged and is never told what it was called, so the name has
 * to be asked for — Get File on slot 0, the ROM slot.
 *
 * The RIA's pro.c also launches ROMs and keeps a launcher chain. Neither
 * exists here: choosing a program is the Pocket's menu reloading the
 * core, so there is nothing for exec to do and nothing to return to.
 */

#include "msc.h"

#include "ria/api/api.h"
#include "ria/api/pro.h"
#include "ria/sys/mem.h"

#include <string.h>

/* A filename, not a path — the assets folder is stripped off — so this
 * is well short of the host's 256-byte field. It is static RAM, and a
 * name too long for it is one nobody can type on a Pocket. */
#define PRO_ARGV0_MAX 128

static char pro_argv0[PRO_ARGV0_MAX];

/* Slot 0's binding cannot change without a core reload, and asking is a
 * blocking bridge command. Once is enough. */
static bool pro_asked;

void pro_run(void)
{
    if (pro_asked)
        return;
    pro_asked = true;
    if (!msc_getfile(0, pro_argv0, sizeof pro_argv0))
        pro_argv0[0] = '\0';
    else if (!memcmp(pro_argv0, MSC_ASSETS_PATH, sizeof MSC_ASSETS_PATH - 1))
        memmove(pro_argv0, pro_argv0 + sizeof MSC_ASSETS_PATH - 1,
                strlen(pro_argv0 + sizeof MSC_ASSETS_PATH - 1) + 1);
}

/* The guest ABI is ria/api/arg.h's: little-endian uint16 offsets into
 * the payload, a {0,0} terminator, then the strings packed behind the
 * table. One string here, so the table is one entry and four bytes.
 *
 * arg.c owns that layout and would be the thing to call, except its
 * buffer is a whole xstack — 512 bytes, spent so exec can carry an argv
 * across a machine reset. There is no exec here, and the firmware has
 * about four kilobytes of stack left between .bss and the top of the
 * TCM. Nothing on this platform can afford an eighth of that to hold
 * one filename; borrowing it is what broke localtime, which needs the
 * deepest stack of anything the machine runs. */
bool pro_api_argv(void)
{
    size_t n = strlen(pro_argv0);
    size_t size = n ? 4 + n + 1 : 2;
    xstack_ptr = XSTACK_SIZE - size;
    uint8_t *p = &xstack[xstack_ptr];
    p[0] = p[1] = 0;
    if (n)
    {
        p[0] = 4;
        p[2] = p[3] = 0;
        memcpy(p + 4, pro_argv0, n + 1);
    }
    return api_return_ax((uint16_t)size);
}
