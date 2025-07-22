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
#include <stdarg.h>
#include "hci_dump.h"
#include "hci_dump_embedded_stdout.h"

#include "classic/hid_host.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "l2cap.h"
#include "bluetooth_data_types.h"
#include "classic/sdp_client.h"

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

    if (packet_type != HCI_EVENT_PACKET)
        return;

    uint8_t event_type = hci_event_packet_get_type(packet);
    bd_addr_t event_addr;
    uint8_t status;

    switch (event_type)
    {
    case BTSTACK_EVENT_STATE:
    {
        uint8_t state = btstack_event_state_get_state(packet);
        printf("BTX: BTSTACK_EVENT_STATE: %d\n", state);
        if (state == HCI_STATE_WORKING)
        {
            printf("BTX: Bluetooth Classic HID Host ready and working!\n");

            // Always re-enable discoverable/connectable when stack becomes ready
            // This is essential because the stack may reset discoverability during initialization
            gap_discoverable_control(1);
            gap_connectable_control(1);
            printf("BTX: Re-enabled discoverable/connectable in working state\n");
        }
        else
        {
            printf("BTX: Bluetooth stack state: %d (not working yet)\n", state);
        }
    }
    break;

    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(packet, event_addr);
        printf("BTX: PIN code request from %s, using '0000'\n", bd_addr_to_str(event_addr));
        gap_pin_code_response(event_addr, "0000");
        break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
        uint32_t numeric_value = hci_event_user_confirmation_request_get_numeric_value(packet);
        printf("BTX: SSP User Confirmation from %s: %lu - Auto accepting\n", bd_addr_to_str(event_addr), (unsigned long)numeric_value);
        // This is CRITICAL for gamepad pairing - must accept the confirmation
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
            printf("BTX: Incoming HID connection from %s\n", bd_addr_to_str(event_addr));

            // Always accept incoming HID connections when discoverable (BTStack pattern)
            hid_host_accept_connection(hid_cid, HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT);
            printf("BTX: Accepting incoming HID connection\n");
        }
        break;

        case HID_SUBEVENT_CONNECTION_OPENED:
        {
            status = hid_subevent_connection_opened_get_status(packet);
            if (status != ERROR_CODE_SUCCESS)
            {
                printf("BTX: HID connection failed, status: 0x%02x\n", status);
                break;
            }

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

                    // We'll wait for HID_SUBEVENT_DESCRIPTOR_AVAILABLE before mounting the gamepad
                    printf("BTX: HID Host connected to gamepad at slot %d, CID: 0x%04x\n", i, hid_cid);
                    printf("BTX: Waiting for HID descriptor before mounting gamepad...\n");

                    // Note: Keep discoverable for multiple device connections
                    // Don't disable discoverability automatically like the old code did
                    printf("BTX: Connection opened successfully! Remaining discoverable for additional devices.\n");
                    break;
                }
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

        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
        {
            uint16_t hid_cid = hid_subevent_descriptor_available_get_hid_cid(packet);
            status = hid_subevent_descriptor_available_get_status(packet);

            // Find the connection for this HID CID
            for (int i = 0; i < PAD_MAX_PLAYERS; i++)
            {
                if (btx_connections[i].active && btx_connections[i].hid_cid == hid_cid)
                {
                    if (status == ERROR_CODE_SUCCESS)
                    {
                        const uint8_t *descriptor = hid_descriptor_storage_get_descriptor_data(hid_cid);
                        uint16_t descriptor_len = hid_descriptor_storage_get_descriptor_len(hid_cid);

                        printf("BTX: HID descriptor available for gamepad at slot %d, length: %d bytes\n", i, descriptor_len);

                        // Now that we have the descriptor, mount the gamepad with it
                        bool mounted = pad_mount(btx_slot_to_pad_idx(i), descriptor, descriptor_len, 0, 0, 0);
                        if (!mounted)
                        {
                            printf("BTX: Failed to mount gamepad at slot %d with descriptor\n", i);
                            break;
                        }

                        printf("BTX: Gamepad successfully mounted with descriptor at slot %d\n", i);
                    }
                    else
                    {
                        printf("BTX: Failed to get HID descriptor for gamepad at slot %d, status: 0x%02x\n", i, status);

                        // Fall back to mounting without a descriptor if we couldn't get one
                        bool mounted = pad_mount(btx_slot_to_pad_idx(i), NULL, 0, 0, 0, 0);
                        if (!mounted)
                        {
                            printf("BTX: Failed to mount gamepad at slot %d with fallback method\n", i);
                            break;
                        }

                        printf("BTX: Gamepad mounted with fallback method at slot %d\n", i);
                    }
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
                    printf("BTX: HID Host disconnected from gamepad at slot %d\n", i);
                    break;
                }
            }
        }
        break;
        }
    }
    break;

    case 0x66: // BTSTACK_EVENT_DISCOVERABLE_ENABLED
    {
        // Event indicates discoverable mode change - this is important for gamepad pairing
        uint8_t discoverable = packet[2]; // Discoverable status is in byte 2
        printf("BTX: Discoverable mode %s\n", discoverable ? "ENABLED" : "DISABLED");
    }
    break;

    default:
        // Don't spam for common events, only log significant ones
        if (event_type != 0x0e && event_type != 0x6e)
        { // Skip command complete and nr connections changed
            DBG("BTX: Unhandled event 0x%02x\n", event_type);
        }
        break;
    }
}

static void btx_init_stack(void)
{
    if (btx_initialized)
        return;

    // BTstack packet logging
    hci_dump_init(hci_dump_embedded_stdout_get_instance());
    hci_dump_enable_packet_log(true);

    // Clear connection array
    memset(btx_connections, 0, sizeof(btx_connections));

    // Note: BTStack memory and run loop are automatically initialized by pico_btstack_cyw43
    // when cyw43_arch_init() is called. We don't need to do it again here.

    // Initialize L2CAP (required for HID Host) - MUST be first
    l2cap_init();
    printf("BTX: L2CAP initialized\n");

    // Initialize SDP Server (needed for service records)
    sdp_init();
    printf("BTX: SDP server initialized\n");

    // Initialize HID Host BEFORE setting GAP parameters
    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
    hid_host_register_packet_handler(btx_packet_handler);
    printf("BTX: HID host initialized and packet handler registered\n");

    // Register for HCI events BEFORE configuring GAP
    hci_event_callback_registration.callback = &btx_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    printf("BTX: HCI event handler registered\n");

    // Create and register SDP record for HID host capability (enhanced for gamepad support)
    // Based on BTStack examples with additional attributes for better gamepad compatibility
    static uint8_t hid_host_sdp_service_buffer[200]; // Increased size for more attributes
    uint32_t hid_host_service_record_handle = sdp_create_service_record_handle();

    de_create_sequence(hid_host_sdp_service_buffer);

    // 0x0000 "Service Record Handle"
    de_add_number(hid_host_sdp_service_buffer, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_SERVICE_RECORD_HANDLE);
    de_add_number(hid_host_sdp_service_buffer, DE_UINT, DE_SIZE_32, hid_host_service_record_handle);

    // 0x0001 "Service Class ID List"
    de_add_number(hid_host_sdp_service_buffer, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_SERVICE_CLASS_ID_LIST);
    uint8_t *attribute = de_push_sequence(hid_host_sdp_service_buffer);
    {
        de_add_number(attribute, DE_UUID, DE_SIZE_16, BLUETOOTH_SERVICE_CLASS_HUMAN_INTERFACE_DEVICE_SERVICE);
    }
    de_pop_sequence(hid_host_sdp_service_buffer, attribute);

    // 0x0005 "Public Browse Group"
    de_add_number(hid_host_sdp_service_buffer, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_BROWSE_GROUP_LIST);
    attribute = de_push_sequence(hid_host_sdp_service_buffer);
    {
        de_add_number(attribute, DE_UUID, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_PUBLIC_BROWSE_ROOT);
    }
    de_pop_sequence(hid_host_sdp_service_buffer, attribute);

    // 0x0100 "Service Name"
    de_add_number(hid_host_sdp_service_buffer, DE_UINT, DE_SIZE_16, 0x0100);
    de_add_data(hid_host_sdp_service_buffer, DE_STRING, 13, (uint8_t *)"RP6502-Console");

    // 0x0101 "Service Description" - Adding this helps with certain gamepads
    de_add_number(hid_host_sdp_service_buffer, DE_UINT, DE_SIZE_16, 0x0101);
    de_add_data(hid_host_sdp_service_buffer, DE_STRING, 20, (uint8_t *)"Game Controller Host");

    // Register the enhanced SDP record
    uint8_t sdp_result = sdp_register_service(hid_host_sdp_service_buffer);
    if (sdp_result == 0)
    {
        printf("BTX: HID host SDP service record registered successfully\n");
    }
    else
    {
        printf("BTX: Failed to register HID host SDP service record, error: 0x%02x\n", sdp_result);
    }

    // Configure GAP for HID Host following BTStack example patterns
    // Set default link policy to allow sniff mode and role switching (from BTStack examples)
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    printf("BTX: Link policy configured for sniff mode and role switching\n");

    // Try to become master on incoming connections (from BTStack example)
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    printf("BTX: Master/slave policy set to prefer master role\n");

    // Set Class of Device for HID support (Joystick + Game Pad)
    // See https://www.bluetooth.com/specifications/assigned-numbers/baseband/
    // 0x002580 = Desktop, Computer, Joystick/Gamepad Minor Device (0x05) with Rendering service (bit 11)
    gap_set_class_of_device(0x002580);
    gap_set_local_name("RP6502-Console");
    printf("BTX: Class of Device (gamepad host) and name configured\n");

    // Enable SSP with simple configuration
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);
    printf("BTX: SSP enabled with auto-accept\n");

    // Make discoverable to allow HID devices to initiate connection (BTStack pattern)
    // This is ESSENTIAL for gamepad pairing - they need to find and connect to us
    gap_discoverable_control(1);
    gap_connectable_control(1);
    printf("BTX: Made discoverable and connectable for HID device connections\n");

    btx_initialized = true;
    printf("BTX: Bluetooth Classic HID Host initialized\n");

    // Start the Bluetooth stack - this should be last
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
}

bool btx_start_pairing(void)
{
    // we are always in pairing mode.
    // DO NOT IMPLEMENT THIS YET!
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
        DBG("  Device discoverable as 'RP6502-Console'\n");
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
