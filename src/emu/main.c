/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/main.h"
#include "emu/api/pro.h"
#include "emu/host/hostdir.h"
#include "emu/sys/mem.h"
#include "emu/sys/xreg.h"
/* Firmware handler decls, ria/-qualified: a bare "api/clk.h"/"api/std.h" from
 * this root file would resolve to the emu/api/ shadows next to it, not the
 * firmware headers that declare the *_api_* op handlers. */
#include "ria/api/api.h"
#include "ria/api/atr.h"
#include "ria/api/std.h"
#include "ria/api/dir.h"
#include "ria/api/clk.h"
#include "ria/str/rln.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* xreg (op 0x01): marshal device/channel/address + words off the xstack */
/* ------------------------------------------------------------------ */

/* The i-th xreg data word (target address+i) sits at xstack[SIZE-5-2i]. */
static uint16_t word_at(int i)
{
    uint16_t word;
    memcpy(&word, &xstack[XSTACK_SIZE - 5 - 2 * i], sizeof(word));
    return word;
}

static bool std_xreg(void)
{
    uint8_t device = xstack[XSTACK_SIZE - 1];
    uint8_t channel = xstack[XSTACK_SIZE - 2];
    uint8_t address = xstack[XSTACK_SIZE - 3];
    int count = (int)((XSTACK_SIZE - xstack_ptr - 3) / 2);
    if ((xstack_ptr & 1) == 0 || count < 1 || count > XSTACK_SIZE / 2 ||
        device > 7 || channel > 15)
    {
        xstack_ptr = XSTACK_SIZE;
        return api_return_errno(API_EINVAL);
    }
    /* VGA control channel ($F) is RIA-private while VGA is connected (always,
     * in the emulator), so a write NAKs (mirrors ria/sys/pix.c). */
    if (device == 1 && channel == 0xF)
    {
        xstack_ptr = XSTACK_SIZE;
        return api_return_errno(API_EACCES);
    }
    /* word[i] lives at xstack[SIZE-5-2i] and targets address+i. Hardware
     * dispatch order: a VGA channel-0 multi-word call starting at address 0
     * sends the canvas word (address 0) first so it can't clear later mode
     * programming; every other word follows high address -> low. This makes
     * a register that consumes earlier ones (e.g. the term mode word at
     * address 1) land after its parameters. */
    bool canvas_first = (device == 1 && channel == 0 && address == 0 && count > 1);
    if (canvas_first && !xreg_write(device, channel, address, word_at(0)))
    {
        xstack_ptr = XSTACK_SIZE;
        return api_return_errno(API_EINVAL);
    }
    for (int i = count - 1; i >= (canvas_first ? 1 : 0); i--)
    {
        /* PIX_DEVICE_RIA (device 0) holds the address constant (last-wins);
         * only the VGA/non-RIA path walks address+i. */
        uint8_t reg = device ? (uint8_t)(address + i) : address;
        if (!xreg_write(device, channel, reg, word_at(i)))
        {
            xstack_ptr = XSTACK_SIZE;
            return api_return_errno(API_EINVAL);
        }
    }
    xstack_ptr = XSTACK_SIZE;
    return api_return_ax(0);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* The 6502 syscall op -> handler table. A runtime array (not a switch) so the dir
 * slots can be swapped between the emu's host handlers and the REAL firmware
 * dir_api_* (ria/api/dir.c) when --tmpdrive mounts a RAM FatFs. The dir slots
 * default to host below; main_dir_ops_set() swaps them. */
typedef bool (*api_op_fn)(void);
static api_op_fn api_ops[0x40] = {
    [0x01] = std_xreg,
    [0x02] = atr_api_phi2,
    [0x03] = atr_api_code_page,
    [0x04] = atr_api_lrand,
    [0x06] = atr_api_errno_opt,
    [0x08] = pro_api_argv,
    [0x09] = pro_api_exec,
    [0x0A] = atr_api_get,
    [0x0B] = atr_api_set,
    [0x14] = std_api_open,
    [0x15] = std_api_close,
    [0x16] = std_api_read_xstack,
    [0x17] = std_api_read_xram,
    [0x18] = std_api_write_xstack,
    [0x19] = std_api_write_xram,
    [0x1A] = std_api_lseek_cc65,
    [0x1B] = hostdir_api_unlink,
    [0x1C] = hostdir_api_rename,
    [0x1D] = std_api_lseek_llvm,
    [0x1E] = std_api_syncfs,
    [0x1F] = hostdir_api_stat,
    [0x20] = hostdir_api_opendir,
    [0x21] = hostdir_api_readdir,
    [0x22] = hostdir_api_closedir,
    [0x23] = hostdir_api_telldir,
    [0x24] = hostdir_api_seekdir,
    [0x25] = hostdir_api_rewinddir,
    [0x26] = hostdir_api_chmod,
    [0x27] = hostdir_api_utime,
    [0x28] = hostdir_api_mkdir,
    [0x29] = hostdir_api_chdir,
    [0x2A] = hostdir_api_chdrive,
    [0x2B] = hostdir_api_getcwd,
    [0x2C] = hostdir_api_setlabel,
    [0x2D] = hostdir_api_getlabel,
    [0x2E] = hostdir_api_getfree,
    [0x30] = rln_api_lastkey,
    [0x31] = rln_api_peek,
    [0x32] = rln_api_poke,
    [0x3A] = clk_api_gmtime,
    [0x3B] = clk_api_localtime,
    [0x3C] = clk_api_mktime,
    [0x3D] = clk_api_strftime,
    [0x3E] = clk_api_time_set,
    [0x3F] = clk_api_time_get,
};

/* Swap the dir op slots: the firmware's own dir_api_* (over the RAM FatFs) when
 * fat, else the emu's host handlers. */
void main_dir_ops_set(bool fat)
{
    static const struct
    {
        uint8_t op;
        api_op_fn host, fat;
    } slots[] = {
        {0x1B, hostdir_api_unlink, dir_api_unlink},
        {0x1C, hostdir_api_rename, dir_api_rename},
        {0x1F, hostdir_api_stat, dir_api_stat},
        {0x20, hostdir_api_opendir, dir_api_opendir},
        {0x21, hostdir_api_readdir, dir_api_readdir},
        {0x22, hostdir_api_closedir, dir_api_closedir},
        {0x23, hostdir_api_telldir, dir_api_telldir},
        {0x24, hostdir_api_seekdir, dir_api_seekdir},
        {0x25, hostdir_api_rewinddir, dir_api_rewinddir},
        {0x26, hostdir_api_chmod, dir_api_chmod},
        {0x27, hostdir_api_utime, dir_api_utime},
        {0x28, hostdir_api_mkdir, dir_api_mkdir},
        {0x29, hostdir_api_chdir, dir_api_chdir},
        {0x2A, hostdir_api_chdrive, dir_api_chdrive},
        {0x2B, hostdir_api_getcwd, dir_api_getcwd},
        {0x2C, hostdir_api_setlabel, dir_api_setlabel},
        {0x2D, hostdir_api_getlabel, dir_api_getlabel},
        {0x2E, hostdir_api_getfree, dir_api_getfree},
    };
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++)
        api_ops[slots[i].op] = fat ? slots[i].fat : slots[i].host;
}

/* The registry api_task dispatches through; the firmware's is the switch in
 * main.c. Returns true if the op has more work (api_working) and should be
 * re-dispatched, false once it has returned to the 6502. */
bool main_api(uint8_t operation)
{
    api_op_fn fn = operation < 0x40 ? api_ops[operation] : NULL;
    return fn ? fn() : api_return_errno(API_ENOSYS);
}
