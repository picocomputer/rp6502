/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/clk.h"
#include "net/ntp.h"
#include "net/wfi.h"
#include "sys/sys.h"
#include "sys/vga.h"
#include "usb/hid.h"
#include "usb/msc.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include <stdio.h>
#include <string.h>

static void sys_print_status(void)
{
    puts(RP6502_NAME);
    if (strlen(RP6502_VERSION))
        puts("RIA Version " RP6502_VERSION);
    else
        puts("RIA " __DATE__ " " __TIME__);
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
    hid_print_status();
    msc_print_status();
}

void sys_init(void)
{
    // Reset terminal.
    puts("\30\33[0m\f");
    // Hello, world.
    sys_print_status();
}
