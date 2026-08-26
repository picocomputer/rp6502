/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/oem.h"
#include "core/api/tim.h"
#include "ria/api/tim.h"
#include "ria/ble/ble.h"
#include "core/hid/kbd.h"
#include "core/hid/kbt.h"
#include "ria/mon/mon.h"
#include "ria/mon/rom.h"
#include "ria/net/cyw.h"
#include "ria/net/wfi.h"
#include "core/str/str.h"
#include "ria/sys/cfg.h"
#include "ria/sys/com.h"
#include "ria/sys/cpu.h"
#include "ria/sys/lfs.h"
#include "ria/sys/mem.h"
#include "ria/sys/vga.h"
#include "ria/usb/nfc.h"
#include "ria/usb/vcp.h"

#include <stdarg.h> /* before pico/printf.h, which uses va_list without it */
#include <pico/printf.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_CFG)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Configuration is a plain ASCII file on the LFS. e.g.
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

#define CFG_VERSION 1

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

/* Must stay a function, not a macro: the settings expand from a directive,
 * and a directive among a function-like macro's arguments is undefined
 * (C11 6.10.3p11). */
static int cfg_printf(struct cfg_sink *sink, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    // vfctprintf is Marco Paland's "Tiny printf" from the Pi Pico SDK
    int result = vfctprintf(cfg_sink_cb, sink, format, va);
    va_end(va);
    return result;
}

static int cfg_emit(struct cfg_sink *sink, const char *opt_str)
{
    return cfg_printf(sink,
                      "+V%u\n"
                      "+P%u\n"
                      "+T%s\n"
                      "+M%s\n"
                      "+S%u\n"
                      "+L%s\n"
                      "+D%u\n"
                      "+N%u\n"
                      "+H%s\n"
#ifdef RP6502_RIA_W
                      "+E%u\n"
                      "+F%s\n"
                      "+W%s\n"
                      "+K%s\n"
                      "+B%u\n"
                      "+O%u\n"
                      "+A%s\n"
#endif /* RP6502_RIA_W */
                      "%s",
                      CFG_VERSION,
                      cpu_get_phi2_khz(),
                      tim_get_time_zone(),
                      str_get_locale(),
                      oem_get_code_page(),
                      kbt_get_layout_list(),
                      vga_get_display_type(),
                      nfc_get_enabled(),
                      vcp_get_nfc_device_hash(),
#ifdef RP6502_RIA_W
                      cyw_get_rf_enable(),
                      cyw_get_rf_country_code(),
                      wfi_get_ssid(),
                      wfi_get_pass(),
                      ble_get_enabled(),
                      com_telnet_get_port(),
                      com_telnet_get_key(),
#endif /* RP6502_RIA_W */
                      opt_str);
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
        lfsresult = cfg_emit(&sink, opt_str);
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
            lfsresult = cfg_emit(&sink, opt_str);
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
        switch (mbuf[1])
        {
        case 'P':
            cpu_load_phi2_khz(str);
            break;
        case 'T':
            tim_load_time_zone(str);
            break;
        case 'M':
            str_load_locale(str);
            break;
        case 'S':
            oem_load_code_page(str);
            break;
        case 'L':
            kbt_load_layout(str);
            break;
        case 'D':
            vga_load_display_type(str);
            break;
        case 'N':
            nfc_load_enabled(str);
            break;
        case 'H':
            vcp_load_nfc_device_hash(str);
            break;
#ifdef RP6502_RIA_W
        case 'E':
            cyw_load_rf_enable(str);
            break;
        case 'F':
            cyw_load_rf_country_code(str);
            break;
        case 'W':
            wfi_load_ssid(str);
            break;
        case 'K':
            wfi_load_pass(str);
            break;
        case 'B':
            ble_load_enabled(str);
            break;
        case 'O':
            com_telnet_load_port(str);
            break;
        case 'A':
            com_telnet_load_key(str);
            break;
#endif /* RP6502_RIA_W */
        default:
            break;
        }
    }
    lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
    mon_add_response_lfs(lfsresult);
}

void __in_flash("cfg_init") cfg_init(void)
{
    cfg_load_with_boot_opt(false);
}

void cfg_save(void)
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
