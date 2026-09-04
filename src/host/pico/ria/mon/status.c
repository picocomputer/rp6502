/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/tim.h"
#include "core/sys/version.h"
#include "ria/api/tim.h"
#include "ria-w/ble/ble.h"
#include "ria/mon/mon.h"
#include "ria/mon/status.h"
#include "ria-w/net/ntp.h"
#include "ria-w/net/wifi.h"
#include "ria/sys/vga.h"
#include "ria/usb/mid.h"
#include "ria/usb/msc.h"
#include "ria/usb/usb.h"
#include "ria/usb/vcp.h"
#include <stdio.h>

__in_flash("STATUS_NAME") static const char STATUS_NAME[] =
    RP6502_NAME "\n";

/* What this board is, beyond the version every machine shares: the radio, and
 * the creator flag that hides a MAC and an SSID. */
#ifdef RP6502_RIA_W
#if RP6502_CREATOR
#define STATUS_BADGE " W+"
#else
#define STATUS_BADGE " W"
#endif
#else
#if RP6502_CREATOR
#define STATUS_BADGE " +"
#else
#define STATUS_BADGE ""
#endif
#endif

/* A responder rather than a queued string, because the stamp is version.c's to
 * know and only the board name and badge are this one's. */
static int status_version_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)state;
    (void)width;
    snprintf(buf, buf_size, "RIA %s" STATUS_BADGE "\n", version_string());
    return -1;
}

void __in_flash("status_add_boot_response") status_add_boot_response(void)
{
    mon_add_response_utf8(STATUS_NAME);
    mon_add_response_fn(status_version_response);
    mon_add_response_fn(vga_boot_response);
}

void status_mon_status(const char *args)
{
    (void)args;
    mon_add_response_utf8(STATUS_NAME);
    mon_add_response_fn(status_version_response);
    mon_add_response_fn(vga_status_response);
#ifdef RP6502_RIA_W
    mon_add_response_fn(wifi_status_response);
    mon_add_response_fn(ntp_status_response);
#endif
    mon_add_response_fn(tim_status_response);
#ifdef RP6502_RIA_W
    mon_add_response_fn(ble_status_response);
#endif
    mon_add_response_fn(usb_status_response);
    mon_add_response_fn(msc_status_response);
    mon_add_response_fn(vcp_status_response);
    mon_add_response_fn(mid_status_response);
}
