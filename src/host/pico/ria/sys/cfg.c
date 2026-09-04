/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/str/oem.h"
#include "core/api/tim.h"
#include "ria/api/tim.h"
#include "ria-w/ble/ble.h"
#include "core/hid/keymap.h"
#include "ria/mon/mon.h"
#include "ria/mon/rom.h"
#include "ria-w/net/cyw.h"
#include "ria-w/net/wifi.h"
#include "core/str/str.h"
#include "ria/sys/cfg.h"
#include "core/sys/config.h"
#include "ria/sys/com.h"
#include "ria/sys/phi2.h"
#include "osal/pico/lfs.h"
#include "ria/sys/mbuf.h"
#include "ria/sys/vga.h"
#include "ria/usb/nfc.h"
#include "ria/usb/vcp.h"


#if defined(DEBUG_SYS) || defined(DEBUG_SYS_CFG)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

/* Configuration is a plain ASCII file on the LFS. The rows are the machine's
 * driver rows -- see core/sys/config.h -- and their order here follows from
 * the roster, which does not matter: a line is found by its letter, so a file
 * written by any build loads on any other. The letters C and R are retired.
 * e.g. */
// +V1         | Version - Must be first
// +P8000      | PHI2
// +C0         | Caps (retired)
// +R0         | RESB (retired)
// +TUTC0      | Time Zone
// +MEN        | Locale
// +S437       | Code Page
// +LUS DE     | Keyboard Layout list
// +D0         | VGA display type
// +N1         | NFC Enabled
// +HvCpHaSh   | NFC VCP Hash
// +E1         | RF Enabled
// +FUS        | RF Country Code
// +WMyWiFi    | WiFi SSID
// +KsEkRiT    | WiFi Password
// +B1         | Bluetooth Enabled
// +O23        | Telnet Port
// +AsEkRiT    | Telnet Key
// BASIC       | Boot ROM - Must be last

/* Every setting, rendered. Called twice: once against the file to find out
 * whether anything actually changed, once to write it if something did. */
struct cfg_sink
{
    lfs_file_t *file;
    int error;
    bool compare; /* reading the file alongside, not writing it */
    bool differs;
};

static void cfg_sink_cb(char character, void *arg)
{
    struct cfg_sink *sink = arg;
    if (sink->error < 0)
        return;
    if (sink->compare)
    {
        char have;
        if (sink->differs)
            return;
        if (lfs_file_read(&lfs_volume, sink->file, &have, 1) != 1 ||
            have != character)
            sink->differs = true;
        return;
    }
    lfs_ssize_t result = lfs_file_write(&lfs_volume, sink->file, &character, 1);
    if (result < 0)
        sink->error = (int)result;
}

/* Every row, then the boot line -- which is not a row: it has no RAM, it is
 * the last line of the file. */
static void cfg_emit(struct cfg_sink *sink, const char *opt_str)
{
    config_render(cfg_sink_cb, sink);
    while (*opt_str)
        cfg_sink_cb(*opt_str++, sink);
}

// Optional string can replace boot string
static void cfg_save_with_boot_opt(const char *opt_str)
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, STR_CFG_FILENAME,
                                     LFS_O_RDWR | LFS_O_CREAT,
                                     &lfs_file_config);
    mon_add_response_lfs(lfsresult);
    if (lfsresult < 0)
        return;
    if (!opt_str)
    {
        opt_str = (char *)mbuf;
        mbuf[0] = 0;
        // Preserve the existing boot line across rewrite
        while (lfs_gets((char *)mbuf, MBUF_SIZE, &lfs_volume, &lfs_file, NULL))
        {
            if (mbuf[0] != '+')
                break;
            mbuf[0] = 0;
        }
    }
    /* Read it back against what we would write. An unchanged config leaves the
     * file untouched -- and untouched means no flash write at all, because
     * opening RDWR|CREAT on an existing file does not mark it dirty and
     * lfs_file_close then has nothing to sync. Only the truncate below would,
     * which is why it cannot happen before this. */
    struct cfg_sink sink = {.file = &lfs_file, .compare = true};
    lfsresult = lfs_file_rewind(&lfs_volume, &lfs_file);
    if (lfsresult >= 0)
        cfg_emit(&sink, opt_str);
    if (lfsresult >= 0 && sink.error < 0)
        lfsresult = sink.error;
    if (lfsresult >= 0 && !sink.differs)
    {
        /* Same bytes, but the file may still be longer than what we rendered. */
        char extra;
        if (lfs_file_read(&lfs_volume, &lfs_file, &extra, 1) == 1)
            sink.differs = true;
    }
    if (lfsresult >= 0 && sink.differs)
    {
        sink.compare = false;
        lfsresult = lfs_file_rewind(&lfs_volume, &lfs_file);
        if (lfsresult >= 0)
            lfsresult = lfs_file_truncate(&lfs_volume, &lfs_file, 0);
        if (lfsresult >= 0)
            cfg_emit(&sink, opt_str);
        if (lfsresult >= 0 && sink.error < 0)
            lfsresult = sink.error;
    }
    mon_add_response_lfs(lfsresult);
    int lfscloseresult = lfs_file_close(&lfs_volume, &lfs_file);
    mon_add_response_lfs(lfscloseresult);
    if (lfsresult < 0 || lfscloseresult < 0)
        lfs_remove(&lfs_volume, STR_CFG_FILENAME);
}

static void cfg_load_with_boot_opt(bool boot_only)
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, STR_CFG_FILENAME,
                                     LFS_O_RDONLY, &lfs_file_config);
    mbuf[0] = 0;
    if (lfsresult < 0)
    {
        if (lfsresult != LFS_ERR_NOENT)
            mon_add_response_lfs(lfsresult);
        return;
    }
    while (lfs_gets((char *)mbuf, MBUF_SIZE, &lfs_volume, &lfs_file, NULL))
    {
        if (mbuf[0] != '+')
            break;
        if (boot_only)
            continue;
        size_t len = strlen((char *)mbuf);
        while (len && mbuf[len - 1] == '\n')
            len--;
        mbuf[len] = 0;
        if (len < 2)
            continue;
        const char *str = (char *)mbuf + 2;
        config_load_line(mbuf[1], str);
    }
    lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
    mon_add_response_lfs(lfsresult);
}

void __in_flash("cfg_init") cfg_init(void)
{
    /* The first thing on this machine to read the volume, so the first that
     * can say the mount went wrong. */
    mon_add_response_lfs(lfs_mount_error);
    cfg_load_with_boot_opt(false);
}

void cfg_file_save(void)
{
    cfg_save_with_boot_opt(NULL);
}

void cfg_save_boot(const char *str)
{
    cfg_save_with_boot_opt(str);
}

const char *cfg_load_boot(void)
{
    cfg_load_with_boot_opt(true);
    return (char *)mbuf;
}
