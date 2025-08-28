/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/clk.h"
#include "net/ble.h"
#include "net/ntp.h"
#include "net/wfi.h"
#include "sys/sys.h"
#include "sys/vga.h"
#include "usb/usb.h"
#include <hardware/watchdog.h>
#include <stdio.h>
#include <string.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_SYS)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static void sys_print_status(void)
{
    puts(RP6502_NAME);
#ifdef RP6502_RIA_W
    if (strlen(RP6502_VERSION))
        puts("RIA Version " RP6502_VERSION " W");
    else
        puts("RIA " __DATE__ " " __TIME__ " W");
#else
    if (strlen(RP6502_VERSION))
        puts("RIA Version " RP6502_VERSION);
    else
        puts("RIA " __DATE__ " " __TIME__);
#endif
}

void sys_mon_reboot(const char *args, size_t len)
{
    (void)(args);
    (void)(len);
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
    sys_print_status();
    vga_print_status();
    wfi_print_status();
    ntp_print_status();
    clk_print_status();
    ble_print_status();
    usb_print_status();
}

void sys_init(void)
{
    // Reset terminal.
    puts("\30\33c");
    // Hello, world.
    sys_print_status();
    if (vga_backchannel())
        vga_print_status();
}
