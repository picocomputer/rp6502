/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See dir.h.
 */

#include "core/api/dir.h"
#include "core/api/api.h"

bool dir_push_filinfo(FILINFO *fno)
{
    /* Push fields in reverse so they land in forward
     * order in the 6502-visible struct. */
    bool ok = true;
    for (int i = FF_LFN_BUF; i >= 0; i--)
        ok &= api_push_char(&fno->fname[i]);
    for (int i = FF_SFN_BUF; i >= 0; i--)
        ok &= api_push_char(&fno->altname[i]);
    ok &= api_push_uint8(&fno->fattrib);
    ok &= api_push_uint16(&fno->crtime);
    ok &= api_push_uint16(&fno->crdate);
    ok &= api_push_uint16(&fno->ftime);
    ok &= api_push_uint16(&fno->fdate);
    uint32_t fsize = fno->fsize;
    if (fno->fsize > 0xFFFFFFFF)
        fsize = 0xFFFFFFFF;
    ok &= api_push_uint32(&fsize);
    return ok;
}
