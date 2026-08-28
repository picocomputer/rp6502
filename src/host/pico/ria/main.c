/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/main.h"
#include "core/api/api.h"
#include "core/api/attr.h"
#include "core/api/clk.h"
#include "core/api/dir.h"
#include "core/str/oem.h"
#include "core/api/proc.h"
#include "core/api/std.h"
#include "core/api/tim.h"
#include "core/aud/aud.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "ria/ble/ble.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include "ria/mon/drive.h"
#include "ria/mon/fil.h"
#include "ria/mon/mon.h"
#include "ria/mon/ram.h"
#include "ria/mon/rom.h"
#include "ria/mon/uf2.h"
#include "ria/net/cyw.h"
#include "ria/net/modem.h"
#include "ria/net/ntp.h"
#include "ria/net/wifi.h"
#include "core/str/rln.h"
#include "core/str/str.h"
#include "ria/sys/com.h"
#include "ria/sys/cfg.h"
#include "ria/sys/cpu.h"
#include "ria/sys/led.h"
#include "ria/sys/lfs.h"
#include "ria/sys/mem.h"
#include "ria/sys/pix.h"
#include "ria/sys/ria.h"
#include "ria/sys/sys.h"
#include "ria/sys/vga.h"
#include "ria/usb/usb.h"
#include "ria/usb/mid.h"
#include "ria/usb/nfc.h"
#include "ria/usb/vcp.h"
#include "ria/usb/xin.h"
#include <pico/stdlib.h>
#include <pico/time.h>
#include <stdio.h>

#ifndef NDEBUG
#define TIME_TASK(fn)                                                  \
    do                                                                 \
    {                                                                  \
        absolute_time_t _t0 = get_absolute_time();                     \
        fn();                                                          \
        int64_t _us = absolute_time_diff_us(_t0, get_absolute_time()); \
        if (_us > 10000)                                               \
            printf("SLOW " #fn " %lldus\n", (long long)_us);           \
    } while (0)
#else
#define TIME_TASK(fn) fn()
#endif

/**************************************/
/* All device drivers register below. */
/**************************************/

// Task events are repeatedly called by the main loop.
// They must not block. All drivers are state machines.

// These tasks run while FatFs is blocking.
// Calling FatFs in here will summon a dragon.
void main_task(void)
{
    TIME_TASK(usb_task);
    std_task();
    cpu_task();
    ria_task();
    keymap_task();
    mid_task();
    cyw_task();
    vga_task();
    com_task();
    wifi_task();
    ntp_task();
    ble_task();
    led_task();
    modem_task();
    ram_task();
}

// Tasks that call FatFs should be here instead of main_task().
static void task(void)
{
    mon_task();
    mem_task();
    rln_task();
    fil_task();
    rom_task();
    uf2_task();
    vcp_task();
    nfc_task(); // must be last for exec
    api_task(); // must be last for exec
}




// Triggered once after init then after every PHI2 change.
void main_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    cpu_reclock();
    ria_reclock(clkdiv_int, clkdiv_frac);
    pix_reclock(clkdiv_int, clkdiv_frac);
}


/*****************************/
/* This is the OS scheduler. */
/*****************************/

static bool is_breaking;

bool lifecycle_break(void)
{
    proc_cancel_launcher();
    is_breaking = true;
    return true;
}

bool lifecycle_break_to_launcher(void)
{
    // From the launcher there is nowhere to return to.
    if (proc_is_launcher())
        return false;
    api_set_ax(0xFFFF);
    is_breaking = true;
    return true;
}

int main(void)
{
    lifecycle_init();
    while (true)
    {
        main_task();
        task();
        if (is_breaking)
            lifecycle_stop();
        lifecycle_commit();
        if (is_breaking)
        {
            lifecycle_break_drivers();
            is_breaking = false;
        }
    }
}
