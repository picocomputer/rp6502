/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The software machines' half of the loader: the install table (the null
 * drive as a map of names to backing files) and the deposit into
 * ram[]/xram[]. The pump these feed is rom_pump.c's, shared by every
 * machine.
 */

#include "core/log.h"
#include "core/api/fs.h"
#include "core/str/path.h"
#include "core/rom/rom.h"
#include "core/rom/rom_rec.h"
#include "host/os.h"
#include "core/mem/mem.h"
#include "core/str/str.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ */
/* Install: map a boot/exec ":name" to its backing host .rp6502        */
/* ------------------------------------------------------------------ */

#define INSTALL_MAX 16
#define INSTALL_NAME_MAX 64

typedef struct
{
    bool used;
    char name[INSTALL_NAME_MAX]; /* basename, e.g. "adventure.rp6502" (the text after ":") */
    char host[HOST_MAX_PATH];    /* the backing file */
} install_t;
static install_t installs[INSTALL_MAX];

/* Install a .rp6502 on the null drive, keyed by its host-path basename. */
bool rom_install(const char *hostpath)
{
    const char *base = path_basename(hostpath);
    if (!*base || strlen(base) >= INSTALL_NAME_MAX || strlen(hostpath) >= HOST_MAX_PATH)
        return false;
    /* Must exist. Asked through the driver, because that is the machine's
     * answer for what a file is. */
    api_errno err;
    int fd = fs_std_open(hostpath, FS_RD, &err);
    if (fd < 0)
        return false;
    fs_std_close(fd, &err);
    for (int i = 0; i < INSTALL_MAX; i++)
        if (!installs[i].used)
        {
            installs[i].used = true;
            strcpy(installs[i].name, base);
            strcpy(installs[i].host, hostpath);
            return true;
        }
    return false;
}

/* Find an installed ROM by name (the text after ":"), case-insensitively to match
 * the firmware's installed-name handling and the sibling ROM: asset driver. */
static install_t *install_find(const char *name)
{
    for (int i = 0; i < INSTALL_MAX; i++)
        if (installs[i].used && strcasecmp(installs[i].name, name) == 0)
            return &installs[i];
    return NULL;
}

/* Resolve a boot/exec ROM path to the file to open: an installed ":name" ->
 * its backing file, anything else verbatim. A drive path and a bare host path
 * are both spellings the filesystem seam accepts, so neither is rewritten
 * here. The loader then opens it. */
bool rom_resolve(const char *path, char *out, size_t outsz)
{
    if (path[0] == ':') /* null drive: an installed ROM, or nothing */
    {
        install_t *in = install_find(path + 1);
        if (!in)
        {
            errno = ENOENT;
            return false;
        }
        if (strlen(in->host) >= outsz)
            return false;
        strcpy(out, in->host);
        return true;
    }
    if (strlen(path) >= outsz)
        return false;
    strcpy(out, path);
    return true;
}

/* ------------------------------------------------------------------ */
/* This machine's loader: pump the records into ram[]/xram[]           */
/* ------------------------------------------------------------------ */

/* Deposit one record's bytes. A load never writes the RIA register window:
 * $FF00-$FFF9 is skipped (the firmware's ria_write_buf does the same over
 * the bus), and the $FFFA-$FFFF vectors land in the register cells too --
 * a load bypasses the bus, so ram[] keeps the shadow every reader uses and
 * regs[] gets the copy the RIA would have taken. */
static void rom_deposit(const rom_rec_t *rec, const uint8_t *buf)
{
    if (rec->addr > 0xFFFF)
    {
        for (uint32_t i = 0; i < rec->len; i++)
            xram[rec->addr - 0x10000 + i] = buf[i]; /* volatile: no memcpy */
        return;
    }
    for (uint32_t i = 0; i < rec->len; i++)
    {
        uint32_t a = rec->addr + i;
        if (a < 0xFF00 || a >= 0xFFFA)
            ram[a] = buf[i];
        if (a >= 0xFFFA && a <= 0xFFFF)
            regs[a & 0x1F] = buf[i];
    }
}

bool rom_load(const char *path)
{
    rom_assets_reset(); /* forget the previous ROM (the MSC0: drive persists) */
    api_errno err;
    rom_pump_t pump;
    if (!rom_pump_open(&pump, path, &err))
    {
        log_error("cannot load ROM '%s'", path);
        return false;
    }
    static uint8_t buf[ROM_REC_MAX];
    rom_rec_t rec;
    rom_pump_result r;
    while ((r = rom_pump_next(&pump, buf, &rec, &err)) != ROM_PUMP_EOF)
    {
        if (r == ROM_PUMP_SKIP)
            continue;
        if (r == ROM_PUMP_ERROR)
        {
            log_error("bad ROM record in '%s'", path);
            rom_pump_close(&pump);
            return false;
        }
        rom_deposit(&rec, buf);
    }
    if (!rom_pump_complete(&pump))
    {
        log_error("ROM has no reset vector ($FFFC/$FFFD)");
        rom_pump_close(&pump);
        return false;
    }
    /* The descriptor and the directory offset become the ROM: drive's: an
     * asset open scans the file for the entry and reads it on demand, so the
     * bytes never enter RAM. */
    rom_asset_adopt(pump.fd, pump.assets_start);
    return true;
}
