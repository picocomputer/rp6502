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
// #include "hci_dump.h"
// #include "hci_dump_embedded_stdout.h"

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
        DBG("BTX: BTSTACK_EVENT_STATE: %d\n", state);
        if (state == HCI_STATE_WORKING)
        {
            DBG("BTX: Bluetooth Classic HID Host ready and working!\n");

            // Always re-enable discoverable/connectable when stack becomes ready
            // This is essential because the stack may reset discoverability during initialization
            gap_discoverable_control(1);
            gap_connectable_control(1);
            DBG("BTX: Re-enabled discoverable/connectable in working state\n");
        }
        else
        {
            DBG("BTX: Bluetooth stack state: %d (not working yet)\n", state);
        }
    }
    break;

    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(packet, event_addr);
        DBG("BTX: PIN code request from %s, using '0000'\n", bd_addr_to_str(event_addr));
        gap_pin_code_response(event_addr, "0000");
        break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
        uint32_t numeric_value = hci_event_user_confirmation_request_get_numeric_value(packet);
        DBG("BTX: SSP User Confirmation from %s: %lu - Auto accepting\n", bd_addr_to_str(event_addr), (unsigned long)numeric_value);
        // This is CRITICAL for gamepad pairing - must accept the confirmation
        gap_ssp_confirmation_response(event_addr);
        break;

    case HCI_EVENT_IO_CAPABILITY_REQUEST:
        hci_event_io_capability_request_get_bd_addr(packet, event_addr);
        DBG("BTX: IO Capability Request from %s - responding with NoInputNoOutput\n", bd_addr_to_str(event_addr));
        // For gamepads, we want to use "NoInputNoOutput" to avoid PIN requirements
        // Note: IO capabilities are set during initialization, this event just confirms the request
        break;

    case HCI_EVENT_INQUIRY_RESULT:
    {
        bd_addr_t addr;
        hci_event_inquiry_result_get_bd_addr(packet, addr);
        uint32_t cod = hci_event_inquiry_result_get_class_of_device(packet);

        DBG("BTX: Inquiry result from %s, CoD: 0x%06lx\n", bd_addr_to_str(addr), (unsigned long)cod);

        // Check if this looks like a gamepad
        // Major device class 0x05 = Peripheral, Minor class bits for joystick/gamepad
        uint8_t major_class = (cod >> 8) & 0x1F;
        uint8_t minor_class = (cod >> 2) & 0x3F;

        DBG("BTX: Device analysis - Major: 0x%02x, Minor: 0x%02x\n", major_class, minor_class);

        if (major_class == 0x05) { // Peripheral device
            // PS4/PS5 controllers often report as pointing devices (0x02) or other peripheral types
            // Accept any peripheral device as a potential gamepad since many controllers
            // don't properly set their minor class to joystick/gamepad
            DBG("BTX: *** FOUND POTENTIAL GAMEPAD! *** %s (CoD: 0x%06lx)\n", bd_addr_to_str(addr), (unsigned long)cod);
            DBG("BTX: Attempting to connect to peripheral device (likely gamepad)...\n");

            // Try to connect to this peripheral device
            hci_send_cmd(&hci_create_connection, addr, 0xCC18, 0x01, 0x00, 0x00, 0x01);
        } else if (major_class == 0x01) {
            DBG("BTX: Found computer device (might be phone/tablet with gamepad app)\n");
        } else {
            DBG("BTX: Device not a peripheral (major class: 0x%02x)\n", major_class);
        }
        break;
    }

    case HCI_EVENT_INQUIRY_COMPLETE:
        DBG("BTX: Inquiry complete\n");
        break;

    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
        status = hci_event_remote_name_request_complete_get_status(packet);
        hci_event_remote_name_request_complete_get_bd_addr(packet, event_addr);
        if (status == 0) {
            char name_buffer[248];
            const char* remote_name = (const char*)hci_event_remote_name_request_complete_get_remote_name(packet);
            int name_len = strlen(remote_name);
            if (name_len > 247) name_len = 247;
            memcpy(name_buffer, remote_name, name_len);
            name_buffer[name_len] = 0;
            DBG("BTX: Remote name for %s: '%s'\n", bd_addr_to_str(event_addr), name_buffer);
        } else {
            DBG("BTX: Remote name request failed for %s, status: 0x%02x\n", bd_addr_to_str(event_addr), status);
        }
        break;

    case HCI_EVENT_CONNECTION_REQUEST:
        hci_event_connection_request_get_bd_addr(packet, event_addr);
        uint32_t class_of_device = hci_event_connection_request_get_class_of_device(packet);
        DBG("BTX: *** INCOMING CONNECTION REQUEST! ***\n");
        DBG("BTX: Connection request from %s, CoD: 0x%06lx\n", bd_addr_to_str(event_addr), (unsigned long)class_of_device);

        // Check if this is a gamepad trying to connect to us
        uint8_t major_class = (class_of_device >> 8) & 0x1F;

        if (major_class == 0x05) { // Peripheral device
            DBG("BTX: This appears to be a gamepad connecting to us!\n");
        }

        // Accept the connection - this is important for gamepad pairing
        hci_send_cmd(&hci_accept_connection_request, event_addr, HCI_ROLE_SLAVE);
        DBG("BTX: Accepting connection as slave device\n");
        break;

    case HCI_EVENT_CONNECTION_COMPLETE:
        status = hci_event_connection_complete_get_status(packet);
        hci_event_connection_complete_get_bd_addr(packet, event_addr);
        if (status == 0) {
            uint16_t handle = hci_event_connection_complete_get_connection_handle(packet);
            DBG("BTX: ACL connection established with %s, handle: 0x%04x\n", bd_addr_to_str(event_addr), handle);

            // Don't force authentication immediately - let the gamepad initiate the HID connection first
            // Some gamepads prefer to establish HID connection before authentication
            DBG("BTX: Waiting for gamepad to initiate HID connection...\n");
        } else {
            DBG("BTX: ACL connection failed with %s, status: 0x%02x\n", bd_addr_to_str(event_addr), status);
        }
        break;

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        status = hci_event_disconnection_complete_get_status(packet);
        if (status == 0) {
            uint16_t handle = hci_event_disconnection_complete_get_connection_handle(packet);
            DBG("BTX: ACL disconnection complete, handle: 0x%04x\n", handle);
        }
        break;

    case HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT:
        status = hci_event_authentication_complete_get_status(packet);
        uint16_t handle = hci_event_authentication_complete_get_connection_handle(packet);
        if (status == 0) {
            DBG("BTX: *** AUTHENTICATION SUCCESSFUL *** for handle: 0x%04x\n", handle);
            DBG("BTX: Gamepad should now be ready for HID connection\n");

            // After successful authentication, we should be ready to accept HID connections
            // The gamepad will now try to connect to our HID service
        } else {
            DBG("BTX: Authentication failed for handle: 0x%04x, status: 0x%02x\n", handle, status);
            DBG("BTX: This may prevent HID connection establishment\n");
        }
        break;

    case HCI_EVENT_ENCRYPTION_CHANGE:
        status = packet[2];
        handle = little_endian_read_16(packet, 3);
        uint8_t encryption_enabled = packet[5];
        if (status == 0) {
            DBG("BTX: Encryption %s for handle: 0x%04x\n",
                   encryption_enabled ? "enabled" : "disabled", handle);
        } else {
            DBG("BTX: Encryption change failed for handle: 0x%04x, status: 0x%02x\n", handle, status);
        }
        break;

    case HCI_EVENT_LINK_KEY_REQUEST:
        hci_event_link_key_request_get_bd_addr(packet, event_addr);
        DBG("BTX: Link key request from %s - sending negative reply\n", bd_addr_to_str(event_addr));
        hci_send_cmd(&hci_link_key_request_negative_reply, event_addr);
        break;

    case HCI_EVENT_LINK_KEY_NOTIFICATION:
        DBG("BTX: Link key notification received\n");
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
            DBG("BTX: *** INCOMING HID CONNECTION *** from %s, CID: 0x%04x\n", bd_addr_to_str(event_addr), hid_cid);

            // Always accept incoming HID connections when discoverable (BTStack pattern)
            hid_host_accept_connection(hid_cid, HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT);
            DBG("BTX: Accepting incoming HID connection with report protocol mode\n");
        }
        break;

        case HID_SUBEVENT_CONNECTION_OPENED:
        {
            status = hid_subevent_connection_opened_get_status(packet);
            if (status != ERROR_CODE_SUCCESS)
            {
                DBG("BTX: HID connection failed, status: 0x%02x\n", status);
                break;
            }

            uint16_t hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            hid_subevent_connection_opened_get_bd_addr(packet, event_addr);

            DBG("BTX: HID_SUBEVENT_CONNECTION_OPENED - CID: 0x%04x, Address: %s\n", hid_cid, bd_addr_to_str(event_addr));

            // Find an empty slot
            for (int i = 0; i < PAD_MAX_PLAYERS; i++)
            {
                if (!btx_connections[i].active)
                {
                    btx_connections[i].active = true;
                    btx_connections[i].hid_cid = hid_cid;
                    memcpy(btx_connections[i].remote_addr, event_addr, BD_ADDR_LEN);

                    DBG("BTX: HID Host connected to gamepad at slot %d, CID: 0x%04x\n", i, hid_cid);
                    DBG("BTX: Waiting for HID descriptor to be automatically retrieved...\n");

                    DBG("BTX: Connection opened successfully! Remaining discoverable for additional devices.\n");
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
                    // Debug: Print first report from each gamepad
                    static bool first_report[PAD_MAX_PLAYERS] = {false};
                    if (!first_report[i]) {
                        first_report[i] = true;
                        DBG("BTX: First HID report from slot %d (CID: 0x%04x), length: %d bytes\n", i, hid_cid, report_len);
                        DBG("BTX: Report data: ");
                        for (int j = 0; j < (report_len < 16 ? report_len : 16); j++) {
                            DBG("%02x ", report[j]);
                        }
                        DBG("\n");
                    }

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

            DBG("BTX: HID_SUBEVENT_DESCRIPTOR_AVAILABLE - CID: 0x%04x, Status: 0x%02x\n", hid_cid, status);

            // Find the connection for this HID CID
            for (int i = 0; i < PAD_MAX_PLAYERS; i++)
            {
                if (btx_connections[i].active && btx_connections[i].hid_cid == hid_cid)
                {
                    if (status == ERROR_CODE_SUCCESS)
                    {
                        const uint8_t *descriptor = hid_descriptor_storage_get_descriptor_data(hid_cid);
                        uint16_t descriptor_len = hid_descriptor_storage_get_descriptor_len(hid_cid);

                        DBG("BTX: HID descriptor available for gamepad at slot %d, length: %d bytes\n", i, descriptor_len);

                        // Print first few bytes of descriptor for debugging
                        if (descriptor && descriptor_len > 0) {
                            DBG("BTX: Descriptor data (first 16 bytes): ");
                            for (int j = 0; j < (descriptor_len < 16 ? descriptor_len : 16); j++) {
                                DBG("%02x ", descriptor[j]);
                            }
                            DBG("\n");
                        }

                        // Now that we have the descriptor, mount the gamepad with it
                        bool mounted = pad_mount(btx_slot_to_pad_idx(i), descriptor, descriptor_len, 0, 0, 0);
                        if (!mounted)
                        {
                            DBG("BTX: Failed to mount gamepad at slot %d with descriptor\n", i);
                            break;
                        }

                        DBG("BTX: Gamepad successfully mounted with descriptor at slot %d\n", i);
                    }
                    else
                    {
                        DBG("BTX: Failed to get HID descriptor for gamepad at slot %d, status: 0x%02x\n", i, status);

                        // Fall back to mounting without a descriptor if we couldn't get one
                        bool mounted = pad_mount(btx_slot_to_pad_idx(i), NULL, 0, 0, 0, 0);
                        if (!mounted)
                        {
                            DBG("BTX: Failed to mount gamepad at slot %d with fallback method\n", i);
                            break;
                        }

                        DBG("BTX: Gamepad mounted with fallback method at slot %d\n", i);
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
                    DBG("BTX: HID Host disconnected from gamepad at slot %d\n", i);
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
        DBG("BTX: Discoverable mode %s\n", discoverable ? "ENABLED" : "DISABLED");
    }
    break;

    case HCI_EVENT_COMMAND_COMPLETE:
    {
        uint16_t opcode = little_endian_read_16(packet, 3);
        uint8_t status = packet[5];

        // Check for scan enable command completion
        if (opcode == 0x0C1A) { // HCI_Write_Scan_Enable
            if (status == 0) {
                DBG("BTX: Scan enable command completed successfully\n");
            } else {
                DBG("BTX: Scan enable command failed with status: 0x%02x\n", status);
            }
        }
        // Check for inquiry command completion
        else if (opcode == 0x0401) { // HCI_Inquiry
            if (status == 0) {
                DBG("BTX: Inquiry command started successfully\n");
            } else {
                DBG("BTX: Inquiry command failed with status: 0x%02x\n", status);
            }
        }
        // Check for inquiry cancel completion
        else if (opcode == 0x0402) { // HCI_Inquiry_Cancel
            DBG("BTX: Inquiry cancel command completed, status: 0x%02x\n", status);
        }
        break;
    }

    case HCI_EVENT_COMMAND_STATUS:
    {
        uint8_t status = packet[2];
        uint16_t opcode = little_endian_read_16(packet, 4);

        if (opcode == 0x0401) { // HCI_Inquiry
            if (status == 0) {
                DBG("BTX: Inquiry command accepted and running\n");
            } else {
                DBG("BTX: Inquiry command rejected with status: 0x%02x\n", status);
            }
        }
        break;
    }

    default:
        // Log all unhandled events to help debug gamepad pairing issues
        // Skip common flow control events but log inquiry-related ones
        if (event_type != 0x6e && event_type != 0x13) { // Skip HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS and HCI_EVENT_NUMBER_OF_COMPLETED_DATA_BLOCKS
            DBG("BTX: Unhandled HCI event 0x%02x (size: %d)\n", event_type, size);
            // Also print the first few bytes for debugging
            DBG("BTX: Event data: ");
            for (int i = 0; i < (size < 8 ? size : 8); i++) {
                DBG("%02x ", packet[i]);
            }
            DBG("\n");
        }
        break;
    }
}

static void btx_init_stack(void)
{
    if (btx_initialized)
        return;

    // BTstack packet logging
    // hci_dump_init(hci_dump_embedded_stdout_get_instance());
    // hci_dump_enable_packet_log(true);

    // Clear connection array
    memset(btx_connections, 0, sizeof(btx_connections));

    // Note: BTStack memory and run loop are automatically initialized by pico_btstack_cyw43
    // when cyw43_arch_init() is called. We don't need to do it again here.

    // Initialize L2CAP (required for HID Host) - MUST be first
    l2cap_init();
    DBG("BTX: L2CAP initialized\n");

    // Initialize SDP Server (needed for service records)
    sdp_init();
    DBG("BTX: SDP server initialized\n");

    // Initialize HID Host BEFORE setting GAP parameters
    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
    hid_host_register_packet_handler(btx_packet_handler);
    DBG("BTX: HID host initialized and packet handler registered\n");

    // Register for HCI events BEFORE configuring GAP
    hci_event_callback_registration.callback = &btx_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    DBG("BTX: HCI event handler registered\n");

    // Configure GAP for HID Host following BTStack example patterns
    // Set default link policy to allow sniff mode and role switching (from BTStack examples)
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    DBG("BTX: Link policy configured for sniff mode and role switching\n");

    // Try to become master on incoming connections (from BTStack example)
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    DBG("BTX: Master/slave policy set to prefer master role\n");

    // Set Class of Device to indicate HID capability
    // 0x002540 = Computer Major Class (0x01), Desktop Minor Class (0x01), with HID service bit set (0x02)
    // This tells gamepads we're a computer that accepts HID connections
    gap_set_class_of_device(0x002540);
    gap_set_local_name("RP6502-Console");
    DBG("BTX: Class of Device (computer with HID) and name configured\n");

    // Configure SSP for modern gamepad compatibility (most PS4/PS5 controllers use SSP)
    gap_ssp_set_enable(1);  // Enable SSP
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    DBG("BTX: SSP enabled with NoInputNoOutput IO capability for gamepad compatibility\n");

    // Set a simple PIN for legacy pairing
    gap_set_bondable_mode(1);
    DBG("BTX: Bondable mode enabled for legacy pairing\n");

    // Make discoverable to allow HID devices to initiate connection (BTStack pattern)
    // This is ESSENTIAL for gamepad pairing - they need to find and connect to us
    gap_discoverable_control(1);
    gap_connectable_control(1);

    // Enable inquiry scan mode explicitly for better gamepad compatibility
    hci_send_cmd(&hci_write_scan_enable, 0x03); // Both inquiry and page scan
    DBG("BTX: Enabled both inquiry and page scan modes\n");

    // Set inquiry scan parameters for better visibility
    // Make inquiry scan more frequent and longer window for better gamepad discovery
    hci_send_cmd(&hci_write_inquiry_scan_activity, 0x1000, 0x0800); // interval=0x1000, window=0x0800
    DBG("BTX: Enhanced inquiry scan parameters for better gamepad discovery\n");

    DBG("BTX: Made discoverable and connectable for HID device connections\n");

    btx_initialized = true;
    DBG("BTX: Bluetooth Classic HID Host initialized\n");

    // Start the Bluetooth stack - this should be last
    hci_power_control(HCI_POWER_ON);
    DBG("BTX: HCI power on command sent\n");
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

    // Periodically ensure we stay discoverable and connectable
    static uint32_t last_status_check = 0;
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (now - last_status_check > 30000) { // Every 30 seconds
        last_status_check = now;

        // Re-enable discoverable/connectable mode to ensure it stays active
        gap_discoverable_control(1);
        gap_connectable_control(1);
        DBG("BTX: Status check - device should be visible as 'RP6502-Console'\n");
        DBG("BTX: CoD: 0x002540 (Computer with HID), Legacy pairing enabled\n");
        DBG("BTX: Advertising HID service - gamepads should see us as HID-capable device\n");
        DBG("BTX: Also performing active gamepad discovery when pairing mode enabled\n");
        DBG("BTX: Try scanning for Bluetooth devices on your phone to verify visibility\n");

        // Also make sure scan modes are still enabled
        hci_send_cmd(&hci_write_scan_enable, 0x03); // Both inquiry and page scan
    }
}

bool btx_start_pairing(void)
{
    if (!btx_initialized) {
        DBG("BTX: Cannot start pairing - not initialized\n");
        return false;
    }

    btx_pairing_mode = true;

    // Make sure we're discoverable and connectable for incoming connections
    gap_discoverable_control(1);
    gap_connectable_control(1);

    DBG("BTX: *** STARTING ACTIVE GAMEPAD SEARCH ***\n");
    DBG("BTX: Put your gamepad in pairing mode now\n");
    DBG("BTX: Device is also discoverable as 'RP6502-Console' for reverse connections\n");

    // Try a simple inquiry first to see if the command works at all
    DBG("BTX: Attempting inquiry with LAP 0x9E8B33, length 0x08 (10.24s), num_responses 0x00 (unlimited)\n");

    // Send inquiry command and check if it gets accepted
    uint8_t result = hci_send_cmd(&hci_inquiry, 0x9E8B33, 0x08, 0x00);
    DBG("BTX: hci_send_cmd returned: %d\n", result);

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
