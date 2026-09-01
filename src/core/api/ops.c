/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See ops.h.
 *
 * A machine that lacks an op does not leave a hole here; it leaves the thing
 * under the handler empty and the handler says ENOSYS for it. The Pocket has
 * no directories to enumerate, and its drive says so once, rather than this
 * file saying it fifteen times.
 *
 * A switch rather than a table of pointers: the emulator kept a table only so
 * its directory slots could be swapped at runtime, and nothing swaps any more.
 * Taking the address of every handler costs a kilobyte of firmware, because it
 * is what stops the compiler folding them in.
 */

#include "core/api/ops.h"
#include "core/api/api.h"
#include "core/api/attr.h"
#include "core/api/clk.h"
#include "core/api/dir.h"
#include "core/api/proc.h"
#include "core/api/std.h"
#include "core/sys/pix.h"
#include "core/str/rln.h"

/* 0x00 (ZXSTACK) and 0xFF (EXIT) are not here: they are answered where the
 * 6502 writes them, before anything is latched, because a machine's answer to
 * "stop" is its own. */
bool ops_dispatch(uint8_t operation)
{
    switch (operation)
    {
    case 0x01:
        return pix_api_xreg();
    case 0x02:
        return attr_api_phi2();
    case 0x03:
        return attr_api_code_page();
    case 0x04:
        return attr_api_lrand();
    case 0x06:
        return attr_api_errno_opt();
    case 0x08:
        return proc_api_argv();
    case 0x09:
        return proc_api_exec();
    case 0x0A:
        return attr_api_get();
    case 0x0B:
        return attr_api_set();
    case 0x0F:
        return clk_api_clock();
    case 0x10:
        return clk_api_get_res();
    case 0x11:
        return clk_api_get_time();
    case 0x12:
        return clk_api_set_time();
    case 0x14:
        return std_api_open();
    case 0x15:
        return std_api_close();
    case 0x16:
        return std_api_read_xstack();
    case 0x17:
        return std_api_read_xram();
    case 0x18:
        return std_api_write_xstack();
    case 0x19:
        return std_api_write_xram();
    case 0x1A:
        return std_api_lseek_cc65();
    case 0x1B:
        return dir_api_unlink();
    case 0x1C:
        return dir_api_rename();
    case 0x1D:
        return std_api_lseek_llvm();
    case 0x1E:
        return std_api_syncfs();
    case 0x1F:
        return dir_api_stat();
    case 0x20:
        return dir_api_opendir();
    case 0x21:
        return dir_api_readdir();
    case 0x22:
        return dir_api_closedir();
    case 0x23:
        return dir_api_telldir();
    case 0x24:
        return dir_api_seekdir();
    case 0x25:
        return dir_api_rewinddir();
    case 0x26:
        return dir_api_chmod();
    case 0x27:
        return dir_api_utime();
    case 0x28:
        return dir_api_mkdir();
    case 0x29:
        return dir_api_chdir();
    case 0x2A:
        return dir_api_chdrive();
    case 0x2B:
        return dir_api_getcwd();
    case 0x2C:
        return dir_api_setlabel();
    case 0x2D:
        return dir_api_getlabel();
    case 0x2E:
        return dir_api_getfree();
    case 0x30:
        return rln_api_lastkey();
    case 0x31:
        return rln_api_peek();
    case 0x32:
        return rln_api_poke();
    case 0x3A:
        return clk_api_gmtime();
    case 0x3B:
        return clk_api_localtime();
    case 0x3C:
        return clk_api_mktime();
    case 0x3D:
        return clk_api_strftime();
    case 0x3E:
        return clk_api_time_set();
    case 0x3F:
        return clk_api_time_get();
    default:
        return api_return_errno(API_ENOSYS);
    }
}
