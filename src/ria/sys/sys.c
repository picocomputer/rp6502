/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/clk.h"
#include "mon/mon.h"
#include "net/ble.h"
#include "net/ntp.h"
#include "net/wfi.h"
#include "str/str.h"
#include "sys/sys.h"
#include "sys/vga.h"
#include "usb/usb.h"
#include "usb/msc.h"
#include <hardware/watchdog.h>
#include <pico/stdio.h>
#include <stdio.h>
#include <string.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_SYS)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

__in_flash("SYS_NAME") static const char SYS_NAME[] =
    RP6502_NAME "\n";

__in_flash("SYS_VERSION") static const char SYS_VERSION[] =
    "RIA "
#if RP6502_VERSION_EMPTY
    __DATE__ " " __TIME__
#else
    "Version " RP6502_VERSION
#endif
#ifdef RP6502_RIA_W
    " W"
#endif
    "\n";

void sys_init(void)
{
#ifdef NDEBUG
    mon_add_response_str(STR_SYS_FULL_TERM_RESET);
#else
    mon_add_response_str(STR_SYS_DEBUG_TERM_RESET);
#endif
    mon_add_response_str(SYS_NAME);
    mon_add_response_str(SYS_VERSION);
    mon_add_response_fn(vga_boot_response);
}

void sys_mon_reboot(const char *args, size_t len)
{
    (void)(args);
    (void)(len);
    stdio_flush();
    watchdog_reboot(0, 0, 0);
}

void sys_mon_reset(const char *args, size_t len)
{
    (void)(args);
    (void)(len);
    main_run();
}

void sys_mon_status(const char *args, size_t len)
{
    (void)(args);
    (void)(len);
    mon_add_response_str(SYS_NAME);
    mon_add_response_str(SYS_VERSION);
    mon_add_response_fn(vga_status_response);
    mon_add_response_fn(wfi_status_response);
    mon_add_response_fn(ntp_status_response);
    mon_add_response_fn(clk_status_response);
    mon_add_response_fn(ble_status_response);
    mon_add_response_fn(usb_status_response);
    mon_add_response_fn(msc_status_response);
}
