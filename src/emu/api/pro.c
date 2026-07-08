/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/api/pro.h"
#include "emu/host/msc.h"
#include "emu/sys/mem.h"
#include "emu/chips/rp6502.h"
#include "emu/sys/cpu.h"
#include "api/api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Survives a machine reset (it must, so EXEC's new argv reaches the new
 * program). */
static uint8_t pro_argv[XSTACK_SIZE];

/* Launcher chain (firmware pro.c): pro_running_path is argv[0] of the program
 * running now; pro_launcher_path is the ROM to re-run when a program exits.
 * pro_exit_code holds the last exit code for the EXIT_CODE attribute. */
static char pro_running_path[MSC_MAX_PATH];
static char pro_launcher_path[MSC_MAX_PATH];
static int16_t pro_exit_code;

/* Pending exec (op 0x09): the new program loads at the frame boundary rather
 * than mid-tick, so the master clock and the partially-run frame stay
 * consistent. pro_exec() captures the ROM path and stops the current program;
 * the frame loop commits it via pro_take_exec(). */
static bool exec_pending;
static char exec_path[MSC_MAX_PATH];

void pro_init(void)
{
    exec_pending = false;
}

void pro_exec(const char *rom_path)
{
    snprintf(exec_path, sizeof(exec_path), "%s", rom_path);
    exec_pending = true;
    cpu_set_halted(true); /* stop the current program; the tick loop exits */
}

const char *pro_take_exec(void)
{
    if (!exec_pending)
        return NULL;
    exec_pending = false;
    return exec_path;
}

static uint16_t pro_argv_count(void)
{
    for (uint16_t i = 0; i < XSTACK_SIZE / 2; i++)
        if (pro_argv[i * 2] == 0 && pro_argv[i * 2 + 1] == 0)
            return i;
    return 0;
}

static void pro_argv_clear(void)
{
    pro_argv[0] = pro_argv[1] = 0;
}

static uint16_t pro_argv_offset_read(uint16_t i)
{
    return pro_argv[i * 2] | ((uint16_t)pro_argv[i * 2 + 1] << 8);
}

static void pro_argv_offset_write(uint16_t i, uint16_t offset)
{
    pro_argv[i * 2] = offset & 0xFF;
    pro_argv[i * 2 + 1] = offset >> 8;
}

static uint16_t pro_argv_size(void)
{
    uint16_t count = pro_argv_count();
    if (count == 0)
        return 2;
    uint16_t offset = pro_argv_offset_read(count - 1);
    return offset + (uint16_t)strlen((const char *)&pro_argv[offset]) + 1;
}

/* Confirm the offset table and packed strings are self-consistent and in
 * bounds — the argv arrives from the 6502, so never trust it. */
static bool pro_argv_validate(void)
{
    uint16_t count = pro_argv_count();
    uint16_t pos = (count + 1) * 2;
    if (pos >= XSTACK_SIZE)
        return false;
    for (uint16_t i = 0; i < count; i++)
    {
        if (pro_argv_offset_read(i) != pos)
            return false;
        while (pos < XSTACK_SIZE && pro_argv[pos] != 0)
            pos++;
        if (pos >= XSTACK_SIZE)
            return false;
        pos++;
    }
    return true;
}

static bool pro_argv_append(const char *str)
{
    uint16_t count = pro_argv_count();
    uint16_t old_strings_start = (count + 1) * 2;
    uint16_t old_size = pro_argv_size();
    uint16_t strings_len = old_size - old_strings_start;
    uint16_t new_str_len = (uint16_t)strlen(str) + 1;
    if (old_size + 2 + new_str_len > XSTACK_SIZE)
        return false;
    memmove(&pro_argv[old_strings_start + 2], &pro_argv[old_strings_start], strings_len);
    for (uint16_t i = 0; i < count; i++)
        pro_argv_offset_write(i, pro_argv_offset_read(i) + 2);
    uint16_t new_offset = old_strings_start + 2 + strings_len;
    pro_argv_offset_write(count, new_offset);
    pro_argv_offset_write(count + 1, 0);
    memcpy(&pro_argv[new_offset], str, new_str_len);
    return true;
}

static const char *pro_argv_index(uint16_t idx)
{
    if (idx >= pro_argv_count())
        return NULL;
    return (const char *)&pro_argv[pro_argv_offset_read(idx)];
}

/* Seed the initially loaded program's argv (firmware rom_load_argv/rom_exec).
 * argv[0] is the program's own path in 6502 form so it can re-exec itself: a
 * drive path or an installed ":name" is used verbatim; a host path maps back
 * through realpath. Empty args are kept, like the monitor's LOAD. */
bool pro_set_argv(const char *rom, int argc, char *const *args)
{
    char abs[MSC_MAX_PATH], msc[MSC_MAX_PATH];
    const char *argv0 = rom;
    if (!msc_has_drive_prefix(rom) && rom[0] != ':' && realpath(rom, abs))
    {
        msc_from_host(abs, msc, sizeof(msc));
        argv0 = msc;
    }
    /* Length-guard each string: pro_argv_append's uint16 math trusts
     * monitor-capped tokens, but host input is unbounded. */
    pro_argv_clear();
    bool ok = strlen(argv0) < XSTACK_SIZE && pro_argv_append(argv0);
    for (int i = 0; ok && i < argc; i++)
        ok = strlen(args[i]) < XSTACK_SIZE && pro_argv_append(args[i]);
    if (!ok)
        pro_argv_clear(); /* no partial argv; the caller decides severity */
    pro_run(); /* the initial program is now what's running */
    return ok;
}

/* Snapshot argv[0] of the program now starting (firmware pro_run), so
 * pro_running_path always names what is running. */
void pro_run(void)
{
    const char *argv0 = pro_argv_index(0);
    snprintf(pro_running_path, sizeof pro_running_path, "%s", argv0 ? argv0 : "");
}

/* LAUNCHER attribute get: is a launcher armed? */
bool pro_has_launcher(void)
{
    return pro_launcher_path[0] != '\0';
}

/* LAUNCHER attribute set: a program registers ITSELF (its argv[0]) as the
 * launcher to re-run after each child exits, or clears the chain. */
void pro_set_launcher(bool is_launcher)
{
    if (is_launcher)
        snprintf(pro_launcher_path, sizeof pro_launcher_path, "%s", pro_running_path);
    else
        pro_launcher_path[0] = '\0';
}

/* True when the program running now is the launcher itself — the chain ends
 * here rather than re-running it forever. */
bool pro_is_launcher(void)
{
    return pro_launcher_path[0] != '\0' &&
           strcmp(pro_running_path, pro_launcher_path) == 0;
}

int16_t pro_get_exit_code(void)
{
    return pro_exit_code;
}

/* Program EXIT (op 0xFF), mirroring firmware pro_stop. Records the exit code
 * and, if a launcher is armed and the exiting program is not itself the
 * launcher, schedules a re-exec of the launcher ROM and returns true (the
 * machine keeps running). Otherwise the chain ends and it returns false, so the
 * caller halts. */
bool pro_exit(int16_t exit_code)
{
    pro_exit_code = exit_code;
    bool relaunch = !pro_is_launcher() && pro_has_launcher();
    pro_running_path[0] = '\0';
    if (!relaunch)
    {
        pro_launcher_path[0] = '\0';
        return false;
    }
    /* Build the relaunch argv from a copy: pro_argv_append overwrites pro_argv,
     * and the launcher path must survive to seed pro_running_path on reload. */
    char path[MSC_MAX_PATH];
    snprintf(path, sizeof path, "%s", pro_launcher_path);
    pro_argv_clear();
    pro_argv_append(path);
    pro_exec(path);
    return true;
}

/* op 0x08: read the running program's argv onto the xstack. */
bool pro_api_argv(void)
{
    uint16_t size = pro_argv_size();
    xstack_ptr = XSTACK_SIZE - size;
    memcpy(&xstack[xstack_ptr], pro_argv, size);
    return api_return_ax(size);
}

/* op 0x09: replace the running program. The xstack holds the new argv buffer;
 * argv[0] names the .rp6502 to load. Committed once validated — load errors
 * surface on reload, matching the firmware. */
bool pro_api_exec(void)
{
    size_t ptr = xstack_ptr;
    uint16_t size = (uint16_t)(XSTACK_SIZE - ptr);
    memcpy(pro_argv, &xstack[ptr], size);
    memset(&pro_argv[size], 0, XSTACK_SIZE - size);
    xstack_ptr = XSTACK_SIZE;
    if (!pro_argv_validate() || !pro_argv_count())
    {
        pro_argv_clear();
        return api_return_errno(API_EINVAL);
    }
    /* argv[0] (an MSC0:/overlay/host name) is resolved by the loader at the
     * frame boundary; a load failure surfaces on reload, matching the firmware. */
    pro_exec(pro_argv_index(0));
    return api_return_ax(0);
}
