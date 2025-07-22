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
#include "pico/time.h"

// BTStack includes
#include "btstack.h"
#include "classic/hid_host.h"

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
static bool btx_pairing_mode = false;

// BTStack state
static btstack_packet_callback_registration_t hci_event_callback_registration;
static bd_addr_t remote_addr;

// Forward declarations
static void btx_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void btx_hid_host_setup(void);

static void btx_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET)
        return;

    uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type)
    {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING)
        {
            DBG("BTX: Bluetooth stack ready\n");
            if (btx_pairing_mode)
            {
                DBG("BTX: Making device discoverable for pairing\n");
                gap_discoverable_control(1);
                gap_connectable_control(1);
            }
        }
        break;

    case HCI_EVENT_PIN_CODE_REQUEST:
        DBG("BTX: Pin code request - using 0000\n");
        hci_event_pin_code_request_get_bd_addr(packet, remote_addr);
        gap_pin_code_response(remote_addr, "0000");
        break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        DBG("BTX: Auto-confirming pairing request\n");
        hci_event_user_confirmation_request_get_bd_addr(packet, remote_addr);
        gap_ssp_confirmation_response(remote_addr);
        break;

    case HCI_EVENT_HID_META:
        switch (hci_event_hid_meta_get_subevent_code(packet))
        {
        case HID_SUBEVENT_CONNECTION_OPENED:
        {
            bd_addr_t event_addr;
            hid_subevent_connection_opened_get_bd_addr(packet, event_addr);
            uint16_t cid = hid_subevent_connection_opened_get_hid_cid(packet);
            uint8_t status = hid_subevent_connection_opened_get_status(packet);

            if (status == ERROR_CODE_SUCCESS)
            {
                // Find a free slot
                for (int i = 0; i < PAD_MAX_PLAYERS; i++)
                {
                    if (!btx_connections[i].active)
                    {
                        btx_connections[i].active = true;
                        DBG("BTX: HID gamepad connected at slot %d, cid=0x%04x\n", i, cid);

                        // TODO: Get HID descriptor and call pad_mount()
                        // For now, just mark as connected
                        break;
                    }
                }

                // Exit pairing mode after successful connection
                if (btx_pairing_mode)
                {
                    btx_pairing_mode = false;
                    gap_discoverable_control(0);
                    DBG("BTX: Pairing mode disabled after successful connection\n");
                }
            }
            else
            {
                DBG("BTX: HID connection failed with status 0x%02x\n", status);
            }
            break;
        }

        case HID_SUBEVENT_CONNECTION_CLOSED:
        {
            uint16_t cid = hid_subevent_connection_closed_get_hid_cid(packet);
            DBG("BTX: HID gamepad disconnected, cid=0x%04x\n", cid);

            // Find and disconnect the slot
            for (int i = 0; i < PAD_MAX_PLAYERS; i++)
            {
                if (btx_connections[i].active)
                {
                    btx_connections[i].active = false;
                    pad_umount(btx_slot_to_pad_idx(i));
                    DBG("BTX: Freed slot %d\n", i);
                    break;
                }
            }
            break;
        }

        case HID_SUBEVENT_REPORT:
        {
            // Process HID report
            const uint8_t *report = hid_subevent_report_get_report(packet);
            uint16_t report_len = hid_subevent_report_get_report_len(packet);

            // Find the slot for this connection
            for (int i = 0; i < PAD_MAX_PLAYERS; i++)
            {
                if (btx_connections[i].active)
                {
                    // TODO: Map cid to slot properly
                    pad_report(btx_slot_to_pad_idx(i), (uint8_t *)report, report_len);
                    break;
                }
            }
            break;
        }
        }
        break;

    default:
        break;
    }
}

static void btx_hid_host_setup(void)
{
    // Initialize HID Host
    hid_host_init(NULL, 0);

    // Register for HCI events
    hci_event_callback_registration.callback = &btx_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // Register for HID events
    hid_host_register_packet_handler(&btx_packet_handler);

    DBG("BTX: HID host setup complete\n");
}

static void btx_init_stack(void)
{
    if (btx_initialized)
        return;

    // Clear connection array
    memset(btx_connections, 0, sizeof(btx_connections));

    // Initialize BTStack HID host services
    btx_hid_host_setup();

    // Set up GAP for device discovery and pairing
    gap_set_class_of_device(0x2540);  // Peripheral, Joystick/Gamepad
    gap_set_local_name("RP6502 RIA"); // TODO nobody will ever see this

    // Enable SSP (Secure Simple Pairing)
    gap_set_bondable_mode(1);
    gap_ssp_set_io_capability(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(0x01); // General bonding, no MITM

    btx_initialized = true;
    DBG("BTX: Bluetooth gamepad infrastructure initialized\n");
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

    // Process BTStack events
    btstack_run_loop_execute();

    // Auto-disable pairing mode after 60 seconds
    static uint32_t pairing_start_time = 0;
    if (btx_pairing_mode)
    {
        if (pairing_start_time == 0)
        {
            pairing_start_time = to_us_since_boot(get_absolute_time()) / 1000;
        }
        else if ((to_us_since_boot(get_absolute_time()) / 1000) - pairing_start_time > 60000)
        {
            btx_pairing_mode = false;
            gap_discoverable_control(0);
            pairing_start_time = 0;
            DBG("BTX: Pairing mode timed out after 60 seconds\n");
            printf("BTX: Pairing mode disabled (timeout)\n");
        }
    }
    else
    {
        pairing_start_time = 0;
    }
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

    // Start pairing mode
    btx_pairing_mode = true;

    // Make device discoverable and connectable for 60 seconds
    gap_discoverable_control(1);
    gap_connectable_control(1);

    // Set inquiry scan and page scan to be more responsive
    hci_send_cmd(&hci_write_inquiry_scan_activity, 0x800, 0x12); // 1.28s interval, 11.25ms window
    hci_send_cmd(&hci_write_page_scan_activity, 0x800, 0x12);    // 1.28s interval, 11.25ms window

    DBG("BTX: Starting Bluetooth gamepad pairing mode\n");
    printf("BTX: Bluetooth gamepad pairing started - put your gamepad in pairing mode now\n");
    printf("BTX: Device is discoverable as 'RP6502 RIA' for 60 seconds\n");
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

    printf("BTX: Bluetooth gamepad support active\n");
    printf("BTX: Pairing mode: %s\n", btx_pairing_mode ? "ON" : "OFF");

    int active_count = 0;
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
    {
        if (btx_connections[i].active)
        {
            active_count++;
            printf("  Slot %d: pad_idx=%d (active)\n", i, btx_slot_to_pad_idx(i));
        }
    }

    if (active_count == 0)
    {
        printf("  No active Bluetooth gamepad connections\n");
    }
    else
    {
        printf("  %d active Bluetooth gamepad connection%s\n", active_count, active_count == 1 ? "" : "s");
    }

    if (btx_pairing_mode)
    {
        printf("  Device discoverable as 'RP6502 RIA'\n");
        printf("  Put your gamepad in pairing mode to connect\n");
    }
    else
    {
        printf("  Use 'set bt' command to start pairing mode\n");
    }
}

void btx_reset(void)
{
    // TODO when called will interrupt pairing session
}

#endif /* RP6502_RIA_W */
