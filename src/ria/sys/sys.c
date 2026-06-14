/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rp6502_version.h"
#include "main.h"
#include "api/clk.h"
#include "api/pro.h"
#include "ble/ble.h"
#include "mon/mon.h"
#include "net/ntp.h"
#include "net/wfi.h"
#include "str/str.h"
#include "sys/sys.h"
#include "sys/vga.h"
#include "usb/mid.h"
#include "usb/msc.h"
#include "usb/usb.h"
#include "usb/vcp.h"
#include <hardware/watchdog.h>
#include <pico/stdio.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_SYS)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

__in_flash("SYS_NAME") static const char SYS_NAME[] =
    RP6502_NAME "\n";

__in_flash("SYS_VERSION") static const char SYS_VERSION[] =
    "RIA " RP6502_VERSION
#ifdef RP6502_RIA_W
    " W"
#if RP6502_CREATOR
    "+"
#endif
#else
#if RP6502_CREATOR
    " +"
#endif
#endif
    "\n";

void sys_init(void)
{
#ifdef NDEBUG
    mon_add_response_utf8(STR_TERM_HARD_RESET);
#else
    // We can't soft reset cursor when ROMs stop because minicom
    // will print the q, but one at startup is fine for debug.
    mon_add_response_utf8("\30\33[0 q");
    mon_add_response_utf8(STR_TERM_SOFT_RESET);
#endif
    mon_add_response_utf8("\n");
    mon_add_response_utf8(SYS_NAME);
    mon_add_response_utf8(SYS_VERSION);
    mon_add_response_fn(vga_boot_response);
    mon_add_response_utf8("\n");
}

void sys_mon_reboot(const char *args)
{
    (void)args;
    stdio_flush();
    watchdog_reboot(0, 0, 0);
}

void sys_mon_reset(const char *args)
{
    (void)args;
    pro_argv_clear();
    main_run();
}

void sys_mon_status(const char *args)
{
    (void)args;
    mon_add_response_utf8(SYS_NAME);
    mon_add_response_utf8(SYS_VERSION);
    mon_add_response_fn(vga_status_response);
    mon_add_response_fn(wfi_status_response);
    mon_add_response_fn(ntp_status_response);
    mon_add_response_fn(clk_status_response);
    mon_add_response_fn(ble_status_response);
    mon_add_response_fn(usb_status_response);
    mon_add_response_fn(msc_status_response);
    mon_add_response_fn(vcp_status_response);
    mon_add_response_fn(mid_status_response);
}
