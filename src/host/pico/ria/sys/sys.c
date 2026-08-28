/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rp6502_version.h"
#include "ria/main.h"
#include "core/api/arg.h"
#include "core/api/proc.h"
#include "core/api/tim.h"
#include "ria/api/tim.h"
#include "ria/ble/ble.h"
#include "ria/mon/mon.h"
#include "ria/net/ntp.h"
#include "ria/net/wifi.h"
#include "core/str/str.h"
#include "ria/sys/sys.h"
#include "ria/sys/vga.h"
#include "ria/usb/mid.h"
#include "ria/usb/msc.h"
#include "ria/usb/usb.h"
#include "ria/usb/vcp.h"
#include <hardware/clocks.h>
#include <hardware/vreg.h>
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

/* First of this machine's drivers. The clock must be up before anything that derives from
 * it (the RIA PIO divider, the audio PWM wrap, the RF band choice). */
void __in_flash("sys_init") sys_init(void)
{
    vreg_set_voltage(SYS_RP2350_VREG);
    set_sys_clock_khz(SYS_RP2350_KHZ, true);
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
    arg_clear();
    mach_run();
}

/* What this machine says it is at boot. The monitor asks; the strings stay
 * here, because they are this driver's to know. */
void __in_flash("sys_add_boot_response") sys_add_boot_response(void)
{
    mon_add_response_utf8(SYS_NAME);
    mon_add_response_utf8(SYS_VERSION);
    mon_add_response_fn(vga_boot_response);
}

void sys_mon_status(const char *args)
{
    (void)args;
    mon_add_response_utf8(SYS_NAME);
    mon_add_response_utf8(SYS_VERSION);
    mon_add_response_fn(vga_status_response);
    mon_add_response_fn(wifi_status_response);
    mon_add_response_fn(ntp_status_response);
    mon_add_response_fn(tim_status_response);
    mon_add_response_fn(ble_status_response);
    mon_add_response_fn(usb_status_response);
    mon_add_response_fn(msc_status_response);
    mon_add_response_fn(vcp_status_response);
    mon_add_response_fn(mid_status_response);
}
