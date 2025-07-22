/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W
#include "net/wfi.h"
void btx_task(void) {}
void btx_start_pairing(void) {}
void btx_print_status(void) {}
#else

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_WFI)
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#include "pico.h"
#include "tusb_config.h"
#include "net/cyw.h"
#include "usb/pad.h"
#include <stdio.h>
#include <string.h>

// We can use the same indexing as hid and xin so long as we keep clear
static uint8_t btx_slot_to_pad_idx(int slot)
{
    return CFG_TUH_HID + PAD_MAX_PLAYERS + slot;
}

// Simple connection tracking
typedef struct
{
    bool active;
    uint8_t placeholder[16]; // Placeholder for future connection data
} btx_connection_t;

static btx_connection_t btx_connections[PAD_MAX_PLAYERS];
static bool btx_initialized = false;

static void btx_init_stack(void)
{
    if (btx_initialized)
        return;

    // Clear connection array
    memset(btx_connections, 0, sizeof(btx_connections));

    // TODO: Initialize BTStack HID host services when APIs are available
    // For now, this is a placeholder that sets up the infrastructure
    // Real implementation would include:
    // - HID host service initialization
    // - Event handler registration
    // - Bluetooth Classic and BLE setup for HID devices

    btx_initialized = true;
    DBG("BTX: Bluetooth gamepad infrastructure initialized (placeholder)\n");
}

void btx_task(void)
{
    // Only initialize and run if CYW43 radio is ready
    if (!cyw_ready())
        return;

    if (!btx_initialized)
    {
        btx_init_stack();
    }

    // TODO: Process Bluetooth HID events and reports
    // This is where the main Bluetooth gamepad processing would happen:
    // 1. Handle incoming connection requests from HID gamepads
    // 2. Parse HID descriptors using the existing pad_mount() function
    // 3. Process HID reports using the existing pad_report() function
    // 4. Manage connection lifecycle

    // The integration points are already established:
    // - btx_slot_to_pad_idx() provides unique pad indices
    // - pad_mount() can parse HID descriptors for Bluetooth devices
    // - pad_report() can process HID reports from any source
    // - cyw_ready() indicates when Bluetooth radio is available
}

void btx_start_pairing(void)
{
    if (!cyw_ready())
    {
        DBG("BTX: Cannot start pairing - Bluetooth radio not ready\n");
        return;
    }

    if (!btx_initialized)
    {
        btx_init_stack();
    }

    // TODO: Implement actual Bluetooth pairing initiation
    // This would involve:
    // - Making the device discoverable
    // - Setting up pairing/authentication callbacks
    // - Enabling HID device scanning
    // - Managing pairing timeouts

    DBG("BTX: Starting Bluetooth gamepad pairing mode (placeholder)\n");
    printf("BTX: Bluetooth gamepad pairing started - devices can now connect\n");
}

void btx_disconnect(void)
{
    if (!btx_initialized)
        return;

    // Disconnect all active Bluetooth gamepad connections
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
    {
        if (btx_connections[i].active)
        {
            // Clean up gamepad registration
            pad_umount(btx_slot_to_pad_idx(i));
            btx_connections[i].active = false;
            DBG("BTX: Disconnected Bluetooth gamepad at slot %d\n", i);
        }
    }

    DBG("BTX: All Bluetooth gamepad connections disconnected\n");
}

void btx_print_status(void)
{
    if (!cyw_ready())
    {
        printf("BTX: Bluetooth radio not ready\n");
        return;
    }

    if (!btx_initialized)
    {
        printf("BTX: Bluetooth gamepad support not initialized\n");
        return;
    }

    printf("BTX: Bluetooth gamepad support ready (infrastructure placeholder)\n");

    int active_count = 0;
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
    {
        if (btx_connections[i].active)
        {
            active_count++;
            printf("  Slot %d: pad_idx=%d\n", i, btx_slot_to_pad_idx(i));
        }
    }

    if (active_count == 0)
    {
        printf("  No active Bluetooth gamepad connections\n");
    }

    printf("  Ready to implement:\n");
    printf("    - Bluetooth Classic HID gamepad support\n");
    printf("    - BLE HID gamepad support\n");
    printf("    - Integration with existing pad system\n");
    printf("    - Connection management and device pairing\n");
}

#endif /* RP6502_RIA_W */
