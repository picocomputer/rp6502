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

#define DEBUG_RIA_NET_BTX

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_BTX)
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
#include "pico/cyw43_arch.h"

// BTStack includes - for Classic HID Host
#include "btstack.h"
#include "classic/hid_host.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "l2cap.h"
#include "bluetooth_data_types.h"

// We can use the same indexing as hid and xin so long as we keep clear
static uint8_t btx_slot_to_pad_idx(int slot)
{
    return CFG_TUH_HID + PAD_MAX_PLAYERS + slot;
}

// Connection tracking for Classic HID Host
typedef struct
{
    bool active;
    uint16_t hid_cid; // BTStack HID connection ID
    bd_addr_t remote_addr;
} btx_connection_t;

static btx_connection_t btx_connections[PAD_MAX_PLAYERS];
static bool btx_initialized = false;
static bool btx_pairing_mode = false;
static absolute_time_t btx_pairing_timeout;

// BTStack state - Classic HID Host
static btstack_packet_callback_registration_t hci_event_callback_registration;

// Storage for HID descriptors - Classic only
static uint8_t hid_descriptor_storage[500]; // HID descriptor storage

// Forward declarations
static void btx_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void btx_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    // Always print this to verify packet handler is being called
    printf("BTX: packet_handler called: type=0x%02x\n", packet_type);

    if (packet_type != HCI_EVENT_PACKET)
        return;

    uint8_t event_type = hci_event_packet_get_type(packet);
    bd_addr_t event_addr;
    uint8_t status;

    // Always print what event we received
    printf("BTX: HCI event received: 0x%02x\n", event_type);

    switch (event_type)
    {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING)
        {
            DBG("BTX: Bluetooth Classic HID Host ready\n");

            if (btx_pairing_mode)
            {
                DBG("BTX: Device is now discoverable for pairing\n");
                gap_discoverable_control(1);
                gap_connectable_control(1);
            }
        }
        else
        {
            DBG("BTX: Bluetooth stack state: %d\n", btstack_event_state_get_state(packet));
        }
        break;

    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(packet, event_addr);
        DBG("BTX: PIN code request, using '0000'\n");
        gap_pin_code_response(event_addr, "0000");
        break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
        uint32_t numeric_value = hci_event_user_confirmation_request_get_numeric_value(packet);
        DBG("BTX: SSP User Confirmation: %lu - Auto accepting\n", (unsigned long)numeric_value);
        gap_ssp_confirmation_response(event_addr);
        break;

    case HCI_EVENT_HID_META:
        {
            uint8_t subevent = hci_event_hid_meta_get_subevent_code(packet);
            switch (subevent)
            {
            case HID_SUBEVENT_INCOMING_CONNECTION:
                {
                    uint16_t hid_cid = hid_subevent_incoming_connection_get_hid_cid(packet);
                    hid_subevent_incoming_connection_get_address(packet, event_addr);
                    DBG("BTX: Incoming HID connection from %s\n", bd_addr_to_str(event_addr));

                    // Accept the connection in Report mode with fallback to Boot mode
                    if (btx_pairing_mode)
                    {
                        hid_host_accept_connection(hid_cid, HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT);
                        DBG("BTX: Accepting incoming HID connection\n");
                    }
                    else
                    {
                        hid_host_decline_connection(hid_cid);
                        DBG("BTX: Declining connection - not in pairing mode\n");
                    }
                }
                break;

            case HID_SUBEVENT_CONNECTION_OPENED:
                {
                    status = hid_subevent_connection_opened_get_status(packet);
                    if (status == ERROR_CODE_SUCCESS)
                    {
                        uint16_t hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
                        hid_subevent_connection_opened_get_bd_addr(packet, event_addr);

                        // Find an empty slot
                        for (int i = 0; i < PAD_MAX_PLAYERS; i++)
                        {
                            if (!btx_connections[i].active)
                            {
                                btx_connections[i].active = true;
                                btx_connections[i].hid_cid = hid_cid;
                                memcpy(btx_connections[i].remote_addr, event_addr, BD_ADDR_LEN);

                                // Mount the gamepad with no descriptor for now (Bluetooth HID)
                                bool mounted = pad_mount(btx_slot_to_pad_idx(i), NULL, 0, 0, 0, 0);
                                if (!mounted)
                                {
                                    DBG("BTX: Failed to mount gamepad at slot %d\n", i);
                                    btx_connections[i].active = false;
                                    break;
                                }

                                DBG("BTX: Gamepad connected at slot %d, HID CID: 0x%04x\n", i, hid_cid);

                                // Exit pairing mode after successful connection
                                if (btx_pairing_mode)
                                {
                                    btx_pairing_mode = false;
                                    gap_discoverable_control(0);
                                    gap_connectable_control(0);
                                    DBG("BTX: Gamepad connected successfully! Pairing mode disabled.\n");
                                }
                                break;
                            }
                        }
                    }
                    else
                    {
                        DBG("BTX: HID connection failed, status: 0x%02x\n", status);
                    }
                }
                break;

            case HID_SUBEVENT_REPORT:
                {
                    uint16_t hid_cid = hid_subevent_report_get_hid_cid(packet);
                    const uint8_t *report = hid_subevent_report_get_report(packet);
                    uint16_t report_len = hid_subevent_report_get_report_len(packet);

                    // Find the connection for this HID CID
                    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
                    {
                        if (btx_connections[i].active && btx_connections[i].hid_cid == hid_cid)
                        {
                            // Process HID report and send to gamepad system
                            pad_report(btx_slot_to_pad_idx(i), report, report_len);
                            break;
                        }
                    }
                }
                break;

            case HID_SUBEVENT_CONNECTION_CLOSED:
                {
                    uint16_t hid_cid = hid_subevent_connection_closed_get_hid_cid(packet);

                    // Find and clean up the connection
                    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
                    {
                        if (btx_connections[i].active && btx_connections[i].hid_cid == hid_cid)
                        {
                            pad_umount(btx_slot_to_pad_idx(i));
                            btx_connections[i].active = false;
                            DBG("BTX: Gamepad disconnected from slot %d\n", i);
                            break;
                        }
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

static void btx_init_stack(void)
{
    if (btx_initialized)
        return;

    // Clear connection array
    memset(btx_connections, 0, sizeof(btx_connections));

    // Note: BTStack memory and run loop are automatically initialized by pico_btstack_cyw43
    // when cyw43_arch_init() is called. We don't need to do it again here.

    // Initialize L2CAP (required for HID Host)
    l2cap_init();

    // Initialize SDP Server
    sdp_init();

    // Initialize HID Host - this is the key component for accepting incoming HID connections
    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
    hid_host_register_packet_handler(btx_packet_handler);
    printf("BTX: HID host initialized and packet handler registered\n");

    // Register for HCI events - must be done before hci_power_control
    hci_event_callback_registration.callback = &btx_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    printf("BTX: HCI event handler registered\n");

    // Configure GAP for HID Host - make discoverable and connectable for incoming connections
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);

    // Allow role switching to accommodate different gamepads
    hci_set_master_slave_policy(0x01); // Allow role switching

    // Set Class of Device to indicate we're a Computer that can accept HID connections
    // Major Device Class: Computer (0x01), Minor: Desktop workstation (0x04)
    // Service Classes: Object Transfer (0x20) + Audio (0x200000)
    gap_set_class_of_device(0x200104); // Computer that accepts HID connections
    gap_set_local_name("RP6502-HID-Host");

    // Enable SSP with automatic accept for simple pairing
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);

    btx_initialized = true;
    DBG("BTX: Bluetooth Classic HID Host initialized\n");
    printf("BTX: About to power on HCI\n");

    // Start the Bluetooth stack
    hci_power_control(HCI_POWER_ON);
    printf("BTX: HCI power on command sent\n");
}

void btx_task(void)
{
    // Only initialize and run if CYW43 radio is ready
    if (!cyw_ready())
        return;

    if (!btx_initialized)
    {
        btx_init_stack();
        return;
    }

    // Check for pairing mode timeout (60 seconds)
    if (btx_pairing_mode && absolute_time_diff_us(btx_pairing_timeout, get_absolute_time()) > 0)
    {
        btx_pairing_mode = false;
        gap_discoverable_control(0);
        gap_connectable_control(0);
        DBG("BTX: Pairing mode timed out after 60 seconds\n");
    }

    // For Pico SDK with CYW43, we need to call cyw43_arch_poll() to handle Bluetooth events
    // The BTStack integration requires this for proper event processing
}

bool btx_start_pairing(void)
{
    if (!cyw_ready())
    {
        DBG("BTX: Bluetooth radio not ready\n");
        return false;
    }

    if (!btx_initialized)
    {
        return false;
    }

    if (btx_pairing_mode)
    {
        // Already in pairing mode, turn it off
        btx_pairing_mode = false;
        gap_discoverable_control(0);
        gap_connectable_control(0);
        DBG("BTX: Pairing mode disabled\n");
    }
    else
    {
        // Start pairing mode with 60 second timeout
        btx_pairing_mode = true;
        btx_pairing_timeout = make_timeout_time_ms(60000); // 60 seconds

        // Make device discoverable and connectable for HID devices
        DBG("BTX: Enabling discoverable mode for gamepad pairing...\n");
        gap_discoverable_control(1);
        gap_connectable_control(1);

        DBG("BTX: Pairing mode enabled for 60 seconds\n");
        DBG("BTX: Device discoverable as 'RP6502-HID-Host'\n");
        DBG("BTX: Put your gamepad in pairing mode now:\n");
        DBG("BTX: - PS4/PS5: Hold Share + PS buttons until light bar flashes rapidly\n");
        DBG("BTX: - Xbox: Hold Xbox button + Pair button until Xbox button flashes rapidly\n");
        DBG("BTX: - Switch Pro: Hold Sync button until lights scroll\n");
        DBG("BTX: - Generic: Check manual for pairing procedure\n");
    }
    return true;
}

void btx_disconnect_all(void)
{
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
    {
        if (btx_connections[i].active)
        {
            // Clean up gamepad registration
            pad_umount(btx_slot_to_pad_idx(i));
            btx_connections[i].active = false;
        }
    }

    DBG("BTX: All Bluetooth gamepad connections disconnected\n");
}

void btx_print_status(void)
{
    if (!cyw_ready())
    {
        DBG("BTX: Bluetooth radio not ready\n");
        return;
    }

    if (!btx_initialized)
    {
        DBG("BTX: Bluetooth gamepad support not initialized\n");
        return;
    }

    DBG("BTX: Bluetooth Classic HID gamepad support active\n");
    DBG("BTX: Pairing mode: %s\n", btx_pairing_mode ? "ON" : "OFF");

    int active_count = 0;
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
    {
        if (btx_connections[i].active)
        {
            active_count++;
            DBG("  Slot %d: pad_idx=%d, HID_CID=0x%04x (active)\n", i, btx_slot_to_pad_idx(i), btx_connections[i].hid_cid);
        }
    }

    if (active_count == 0)
    {
        DBG("  No active Bluetooth gamepad connections\n");
    }
    else
    {
        DBG("  %d active Bluetooth gamepad connection%s\n", active_count, active_count == 1 ? "" : "s");
    }

    if (btx_pairing_mode)
    {
        DBG("  Device discoverable as 'RP6502-HID-Host'\n");
        DBG("  Put your gamepad in pairing mode to connect\n");
    }
    else
    {
        DBG("  Use 'set bt' command to start pairing mode\n");
    }
}

void btx_reset(void)
{
    // TODO when called will interrupt pairing session
}

#endif /* RP6502_RIA_W */
