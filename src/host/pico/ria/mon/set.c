/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/str/oem.h"
#include "core/api/tim.h"
#include "ria/api/tim.h"
#include "ria-w/ble/ble.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "ria/mon/mon.h"
#include "ria/mon/rom.h"
#include "ria/mon/set.h"
#include "ria-w/net/cyw.h"
#include "ria-w/net/wifi.h"
#include "core/str/str.h"
#include "core/sys/config.h"
#include "ria/sys/com.h"
#include "ria/sys/cfg.h"
#include "ria/sys/phi2.h"
#include "ria/sys/vga.h"
#include "ria/usb/nfc.h"
#include <stdio.h>
#include <pico.h>

static int set_boot_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    const char *rom = rom_get_boot();
    if (!rom[0])
        rom = S(STR_PARENS_NONE);
    snprintf(buf, buf_size, STR_SET_BOOT_RESPONSE, rom);
    return -1;
}

static void set_boot(const char *args)
{
    if (*args)
    {
        const char *scan = args;
        const char *tok = str_parse_string(&scan);
        if (tok && !strcmp(tok, "-") && str_parse_end(scan) && *args != '"')
            rom_set_boot("");
        else
        {
            if (!rom_set_boot(args))
                return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        }
    }
    mon_add_response_fn(set_boot_response);
}

static void set_string(const char *args, bool (*set)(const char *),
                       int (*resp)(char *, size_t, int, unsigned))
{
    if (*args)
    {
        const char *scan = args;
        const char *tok = str_parse_string(&scan);
        if (tok && !strcmp(tok, "-") && str_parse_end(scan) && *args != '"')
            tok = "";
        else if (!tok || !str_parse_end(scan))
            tok = NULL;
        if (!tok || !set(tok))
            return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    }
    mon_add_response_fn(resp);
}

#define DRIVER(i, t, iot, r, s, b, c1, c2, ...) c1 c2
#define CONFIG_INT(ltr, pfx, name, type, def, check, apply, attr, resp, ...) \
    static void set_mon_##pfx##_##name(const char *args)                     \
    {                                                                        \
        uint32_t val;                                                        \
        if (*args && (!str_parse_uint32(&args, &val) ||                      \
                      !str_parse_end(args) ||                                \
                      val != (uint32_t)(type)val ||                          \
                      !pfx##_set_##name((type)val)))                         \
            return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));       \
        mon_add_response_fn(resp);                                           \
    }
#define CONFIG_STR(ltr, pfx, name, size, def, check, apply, attr, resp, ...) \
    static void set_mon_##pfx##_##name(const char *args)                    \
    {                                                                       \
        set_string(args, pfx##_set_##name, resp);                           \
    }
#define CONFIG_RAW(ltr, pfx, name, size, def, check, apply, attr, resp, ...) \
    static void set_mon_##pfx##_##name(const char *args)                    \
    {                                                                       \
        if (*args && !pfx##_set_##name(args))                               \
            return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));      \
        mon_add_response_fn(resp);                                          \
    }
#define CONFIG_HIDDEN(...)
#define CONFIG_SAVE(fn)
DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef CONFIG_SAVE
#undef CONFIG_HIDDEN
#undef CONFIG_RAW
#undef CONFIG_STR
#undef CONFIG_INT
#undef DRIVER

void set_mon_set(const char *args)
{
    if (*args)
    {
        const char *word = str_parse_string(&args);
        if (word)
        {
            if (!strcasecmp(word, STR_BOOT))
                return set_boot(args);
#define DRIVER(i, t, iot, r, s, b, c1, c2, ...) c1 c2
#define CONFIG_INT(ltr, pfx, name, type, def, check, apply, attr, ...) \
    if (!strcasecmp(word, attr))                                       \
        return set_mon_##pfx##_##name(args);
#define CONFIG_STR CONFIG_INT
#define CONFIG_RAW CONFIG_INT
#define CONFIG_HIDDEN(...)
#define CONFIG_SAVE(fn)
            DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef CONFIG_SAVE
#undef CONFIG_HIDDEN
#undef CONFIG_RAW
#undef CONFIG_STR
#undef CONFIG_INT
#undef DRIVER
        }
        return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    }

    mon_add_response_fn(phi2_response);
    mon_add_response_fn(set_boot_response);
    mon_add_response_fn(tim_time_zone_response);
    mon_add_response_fn(str_locale_response);
    mon_add_response_fn(keymap_layout_list_response);
    mon_add_response_fn(oem_code_page_response);
    mon_add_response_fn(vga_display_type_response);
    mon_add_response_fn(nfc_enabled_response);
#ifdef RP6502_RIA_W
    mon_add_response_fn(cyw_rf_enable_response);
    mon_add_response_fn(cyw_rf_country_code_response);
    mon_add_response_fn(wifi_ssid_response);
    mon_add_response_fn(com_telnet_port_response);
    mon_add_response_fn(ble_enabled_response);
#endif
}
