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
#include "btstack.h"

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
    uint16_t acl_handle; // ACL connection handle
} btx_connection_t;

static btx_connection_t btx_connections[PAD_MAX_PLAYERS];
static bool btx_initialized = false;
static bool btx_pairing_mode = false;

// BTStack state - Classic HID Host
static btstack_packet_callback_registration_t hci_event_callback_registration;

// Storage for HID descriptors - Classic only
static uint8_t hid_descriptor_storage[500]; // HID descriptor storage

static int find_connection_by_handle(uint16_t handle)
{
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
    {
        if (btx_connections[i].active && btx_connections[i].acl_handle == handle)
        {
            return i;
        }
    }
    return -1;
}

static int find_connection_by_addr(bd_addr_t addr)
{
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
    {
        if (btx_connections[i].active && memcmp(btx_connections[i].remote_addr, addr, BD_ADDR_LEN) == 0)
        {
            return i;
        }
    }
    return -1;
}

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
        uint8_t state = btstack_event_state_get_state(packet);
        DBG("BTX: BTSTACK_EVENT_STATE: %d\n", state);
        if (state == HCI_STATE_WORKING)
        {
            DBG("BTX: Bluetooth Classic HID Host ready and working!\n");
            // Always re-enable discoverable/connectable when stack becomes ready
            // This is essential because the stack may reset discoverability during initialization
            gap_discoverable_control(1);
            gap_connectable_control(1);
            // DBG("BTX: Re-enabled discoverable/connectable in working state\n");
        }
        else
        {
            DBG("BTX: Bluetooth stack state: %d (not working yet)\n", state);
        }
        break;

    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(packet, event_addr);
        const char *pin = "0000"; // Default fallback
        DBG("BTX: PIN code request from %s, using default PIN '%s'\n", bd_addr_to_str(event_addr), pin);
        gap_pin_code_response(event_addr, pin);
        break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
        uint32_t numeric_value = hci_event_user_confirmation_request_get_numeric_value(packet);
        DBG("BTX: SSP User Confirmation from %s: %lu - Auto accepting\n", bd_addr_to_str(event_addr), (unsigned long)numeric_value);
        // This is CRITICAL for gamepad pairing - must accept the confirmation
        gap_ssp_confirmation_response(event_addr);
        break;

    case HCI_EVENT_USER_PASSKEY_REQUEST:
        hci_event_user_passkey_request_get_bd_addr(packet, event_addr);
        DBG("BTX: User passkey requested from %s - auto responding with '0000'\n", bd_addr_to_str(event_addr));
        // For Classic Bluetooth, respond directly with the HCI command
        hci_send_cmd(&hci_user_passkey_request_reply, event_addr, 0);
        break;

    case HCI_EVENT_IO_CAPABILITY_REQUEST:
        hci_event_io_capability_request_get_bd_addr(packet, event_addr);

        // Check if we have stored device capabilities from a previous IO Capability Response
        // int conn_slot = find_connection_by_addr(event_addr);
        uint8_t io_capability;
        uint8_t oob_data_present;
        uint8_t auth_requirement;

        // Use settings optimized for gamepad pairing
        io_capability = SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT;                           // NoInputNoOutput is best for gamepad compatibility
        oob_data_present = 0x00;                                                        // No OOB data initially
        auth_requirement = SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING; // Use general bonding

        DBG("BTX: HCI_EVENT_IO_CAPABILITY_REQUEST\n");

        hci_send_cmd(&hci_io_capability_request_reply, event_addr, io_capability, oob_data_present, auth_requirement);
        break;

    case HCI_EVENT_INQUIRY_COMPLETE:
        DBG("BTX: Inquiry complete\n");
        break;

    case HCI_EVENT_INQUIRY_RESULT:
    {
        bd_addr_t addr;
        hci_event_inquiry_result_get_bd_addr(packet, addr);
        uint32_t cod = hci_event_inquiry_result_get_class_of_device(packet);
        DBG("BTX: Found device %s (CoD: 0x%06lx) - attempting connection\n", bd_addr_to_str(addr), (unsigned long)cod);
        // hci_send_cmd(&hci_inquiry_cancel);
        hci_send_cmd(&hci_create_connection, addr, 0xCC18, 0x01, 0x00, 0x00, 0x01);
        break;
    }

    case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
    {
        bd_addr_t addr;
        // For inquiry result with RSSI, BD_ADDR is at offset 3-8, similar to basic inquiry
        hci_event_inquiry_result_get_bd_addr(packet, addr);
        uint32_t cod = hci_event_inquiry_result_get_class_of_device(packet);
        int8_t rssi = (int8_t)packet[15]; // RSSI is typically at the end

        DBG("BTX: Inquiry result with RSSI from %s, CoD: 0x%06lx, RSSI: %d dBm\n",
            bd_addr_to_str(addr), (unsigned long)cod, rssi);

        // Try to connect to this device
        // hci_send_cmd(&hci_inquiry_cancel);
        hci_send_cmd(&hci_create_connection, addr, 0xCC18, 0x01, 0x00, 0x00, 0x01);
        break;
    }

    case HCI_EVENT_EXTENDED_INQUIRY_RESPONSE:
    {
        bd_addr_t addr;
        // For extended inquiry result, the BD_ADDR is at offset 3-8
        memcpy(addr, packet + 3, BD_ADDR_LEN);
        uint32_t cod = little_endian_read_24(packet, 9); // Class of Device at offset 9-11
        // uint8_t rssi = packet[2];                        // RSSI at offset 2

        // Check for HID service bit (bit 13 in CoD)
        bool has_hid_service = (cod & (1 << 13)) != 0;
        DBG("BTX: HID service bit: %s\n", has_hid_service ? "Present" : "Not present");

        // Don't try to guess what's a gamepad from Class of Device
        // Just connect to everything and let pad_mount() determine if it's actually a gamepad
        DBG("BTX: Found device %s (CoD: 0x%06lx) via extended inquiry - attempting connection\n", bd_addr_to_str(addr), (unsigned long)cod);
        DBG("BTX: Will determine if it's a gamepad after HID connection is established\n");

        // Try to connect to this device
        // hci_send_cmd(&hci_inquiry_cancel);
        hci_send_cmd(&hci_create_connection, addr, 0xCC18, 0x01, 0x00, 0x00, 0x01);
        break;
    }

    case HCI_EVENT_CONNECTION_REQUEST:
        hci_event_connection_request_get_bd_addr(packet, event_addr);
        uint32_t class_of_device = hci_event_connection_request_get_class_of_device(packet);
        DBG("BTX: Connection request from %s, CoD: 0x%06lx\n", bd_addr_to_str(event_addr), (unsigned long)class_of_device);

        // Decode the incoming device's Class of Device
        uint8_t major_class = (class_of_device >> 8) & 0x1F;
        // uint8_t minor_class = (class_of_device >> 2) & 0x3F;
        {
            bool has_hid_service = (class_of_device & (1 << 13)) != 0;
            if (!has_hid_service && major_class != 0x05)
            {
                if (major_class != 0x08 && major_class != 0x00 && major_class != 0x04)
                {
                    // DBG("BTX: Rejecting connection - device does not advertise HID service and is not a likely gamepad\n");
                    // hci_send_cmd(&hci_reject_connection_request, event_addr, ERROR_CODE_CONNECTION_REJECTED_DUE_TO_LIMITED_RESOURCES);
                    // break;
                }
            }
        }
        DBG("BTX: Accepting connection from potential gamepad device\n");
        // HCI_ROLE_MASTER or HCI_ROLE_SLAVE
        hci_send_cmd(&hci_accept_connection_request, event_addr, HCI_ROLE_MASTER);
        break;

    case HCI_EVENT_CONNECTION_COMPLETE:
        status = hci_event_connection_complete_get_status(packet);
        hci_event_connection_complete_get_bd_addr(packet, event_addr);
        if (status == 0)
        {
            uint16_t handle = hci_event_connection_complete_get_connection_handle(packet);
            DBG("BTX: ACL connection established with %s, handle: 0x%04x\n", bd_addr_to_str(event_addr), handle);

            // Find an empty slot to track this connection
            int slot = -1;
            for (int i = 0; i < PAD_MAX_PLAYERS; i++)
            {
                if (!btx_connections[i].active)
                {
                    slot = i;
                    break;
                }
            }

            if (slot >= 0)
            {
                btx_connections[slot].active = true;
                btx_connections[slot].acl_handle = handle;
                memcpy(btx_connections[slot].remote_addr, event_addr, BD_ADDR_LEN);
                btx_connections[slot].hid_cid = 0;

                DBG("BTX: Device: %s, Handle: 0x%04x (slot %d) - initiating HID connection\n", bd_addr_to_str(event_addr), handle, slot);

                // Initiate HID connection to the device (most gamepads expect host to connect)
                uint8_t hid_status = hid_host_connect(event_addr,
                                                      HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT,
                                                      &btx_connections[slot].hid_cid);
                if (hid_status == ERROR_CODE_SUCCESS)
                {
                    DBG("BTX: HID connection initiated to %s, HID CID: 0x%04x\n",
                        bd_addr_to_str(event_addr), btx_connections[slot].hid_cid);
                }
                else
                {
                    DBG("BTX: Failed to initiate HID connection to %s, status: 0x%02x\n",
                        bd_addr_to_str(event_addr), hid_status);
                    // Clear HID CID but keep ACL connection tracking active for proper cleanup
                    btx_connections[slot].hid_cid = 0;
                    DBG("BTX: HID connection initiation failed, ACL connection still active\n");
                }
            }
            else
            {
                DBG("BTX: No free slots to track connection - this may cause issues\n");
            }
        }
        break;

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        status = hci_event_disconnection_complete_get_status(packet);
        if (status == 0)
        {
            uint16_t handle = hci_event_disconnection_complete_get_connection_handle(packet);
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);

            // Find the connection to get the address for better debugging
            int slot = find_connection_by_handle(handle);
            const char *device_addr = "Unknown";
            if (slot >= 0)
            {
                device_addr = bd_addr_to_str(btx_connections[slot].remote_addr);
                DBG("BTX: ACL disconnection complete, handle: 0x%04x, reason: 0x%02x, device: %s (slot %d)\n",
                    handle, reason, device_addr, slot);
            }
            else
            {
                DBG("BTX: ACL disconnection complete, handle: 0x%04x, reason: 0x%02x, device: %s\n",
                    handle, reason, device_addr);
                DBG("BTX: Connection slot not found - checking all active connections:\n");
                for (int i = 0; i < PAD_MAX_PLAYERS; i++)
                {
                    if (btx_connections[i].active)
                    {
                        DBG("BTX:   Slot %d: handle=0x%04x, addr=%s, hid_cid=0x%04x\n",
                            i, btx_connections[i].acl_handle, bd_addr_to_str(btx_connections[i].remote_addr), btx_connections[i].hid_cid);
                    }
                }
            }

            // Clean up connection tracking
            if (slot >= 0)
            {
                DBG("BTX: Cleaning up connection tracking for slot %d (was HID_CID: 0x%04x)\n", slot, btx_connections[slot].hid_cid);
                if (btx_connections[slot].hid_cid != 0)
                {
                    // HID connection should be cleaned up by HID_SUBEVENT_CONNECTION_CLOSED
                    // but make sure gamepad is unmounted
                    DBG("BTX: Warning: HID connection was not properly closed before ACL disconnection\n");
                    pad_umount(btx_slot_to_pad_idx(slot));
                }
                btx_connections[slot].active = false;
                btx_connections[slot].hid_cid = 0;

                DBG("BTX: Connection slot %d fully cleaned up and available for reuse\n", slot);
            }
        }
        else
        {
            DBG("BTX: Disconnection complete event failed, status: 0x%02x\n", status);
        }
        break;

    case HCI_EVENT_AUTHENTICATION_COMPLETE:
        DBG("BTX: HCI_EVENT_AUTHENTICATION_COMPLETE\n");
        break;

    case HCI_EVENT_HID_META:
    {
        uint8_t subevent = hci_event_hid_meta_get_subevent_code(packet);
        // DBG("BTX: HID META EVENT - Subevent: 0x%02x\n", subevent);

        switch (subevent)
        {
        case HID_SUBEVENT_INCOMING_CONNECTION:
        {
            uint16_t hid_cid = hid_subevent_incoming_connection_get_hid_cid(packet);
            hid_subevent_incoming_connection_get_address(packet, event_addr);
            DBG("BTX: *** INCOMING HID CONNECTION *** from %s, CID: 0x%04x\n", bd_addr_to_str(event_addr), hid_cid);

            // Find the existing ACL connection and store the hid_cid
            int slot = find_connection_by_addr(event_addr);
            if (slot >= 0)
            {
                btx_connections[slot].hid_cid = hid_cid;
                DBG("BTX: Stored HID CID 0x%04x for connection slot %d\n", hid_cid, slot);
            }

            // Always accept incoming HID connections when discoverable (BTStack pattern)
            hid_host_accept_connection(hid_cid, HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT);
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
                    if (!first_report[i])
                    {
                        first_report[i] = true;
                        DBG("BTX: First HID report from slot %d (CID: 0x%04x), length: %d bytes\n", i, hid_cid, report_len);
                        DBG("BTX: Report data: ");
                        for (int j = 0; j < (report_len < 16 ? report_len : 16); j++)
                        {
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

                        DBG("BTX: HID descriptor available for device at slot %d, length: %d bytes\n", i, descriptor_len);

                        // Print first few bytes of descriptor for debugging
                        if (descriptor && descriptor_len > 0)
                        {
                            DBG("BTX: Descriptor data (first 16 bytes): ");
                            for (int j = 0; j < (descriptor_len < 16 ? descriptor_len : 16); j++)
                            {
                                DBG("%02x ", descriptor[j]);
                            }
                            DBG("\n");
                        }

                        // Try to mount the device - pad_mount will return true only for actual gamepads
                        bool mounted = pad_mount(btx_slot_to_pad_idx(i), descriptor, descriptor_len, 0, 0, 0);
                        if (mounted)
                        {
                            DBG("BTX: *** GAMEPAD CONFIRMED! *** Successfully mounted at slot %d\n", i);
                        }
                        else
                        {
                            DBG("BTX: Device at slot %d is NOT a gamepad - pad_mount returned false\n", i);
                            // Clean up the HID connection since it's not a gamepad, but keep ACL connection tracking
                            hid_host_disconnect(hid_cid);
                            btx_connections[i].hid_cid = 0; // Clear HID CID but keep connection active for ACL cleanup
                            DBG("BTX: HID connection disconnected for non-gamepad device, ACL connection still active\n");
                        }
                    }
                    else
                    {
                        DBG("BTX: Failed to get HID descriptor for device at slot %d, status: 0x%02x\n", i, status);
                    }
                    break;
                }
            }
        }
        break;

        case HID_SUBEVENT_CONNECTION_OPENED:
            DBG("BTX: HID_SUBEVENT_CONNECTION_OPENED\n");

            uint16_t hid_cid = hid_subevent_connection_closed_get_hid_cid(packet);

            // Find and clean up the connection
            for (int i = 0; i < PAD_MAX_PLAYERS; i++)
            {
                if (btx_connections[i].active && btx_connections[i].hid_cid == hid_cid)
                {
                    // hci_send_cmd(&hci_authentication_requested, btx_connections[i].acl_handle);
                    break;
                }
            }

            break;

        case HID_SUBEVENT_CONNECTION_CLOSED:
        {
            uint16_t hid_cid = hid_subevent_connection_closed_get_hid_cid(packet);
            DBG("BTX: HID_SUBEVENT_CONNECTION_CLOSED (0x03) - CID: 0x%04x\n", hid_cid);

            // Find the connection and clean up HID-specific resources
            for (int i = 0; i < PAD_MAX_PLAYERS; i++)
            {
                if (btx_connections[i].active && btx_connections[i].hid_cid == hid_cid)
                {
                    pad_umount(btx_slot_to_pad_idx(i));
                    btx_connections[i].hid_cid = 0; // Clear HID CID but keep connection active
                    DBG("BTX: HID connection closed for slot %d (ACL connection still active, awaiting ACL disconnection)\n", i);
                    break;
                }
            }
        }
        break;

        case HID_SUBEVENT_SNIFF_SUBRATING_PARAMS:
        {
            uint16_t hid_cid = little_endian_read_16(packet, 3); // CID is typically at offset 3
            DBG("BTX: HID_SUBEVENT_SNIFF_SUBRATING_PARAMS (0x0e) - CID: 0x%04x (power management event)\n", hid_cid);
            // This is a power management event, no action needed
        }
        break;

        default:
            // Provide context for common subevents
            const char *subevent_name = "Unknown";
            switch (subevent)
            {
            case 0x04:
                subevent_name = "CAN_SEND_NOW";
                break;
            case 0x05:
                subevent_name = "SUSPEND";
                break;
            case 0x06:
                subevent_name = "EXIT_SUSPEND";
                break;
            case 0x07:
                subevent_name = "VIRTUAL_CABLE_UNPLUG";
                break;
            case 0x08:
                subevent_name = "GET_REPORT_RESPONSE";
                break;
            case 0x09:
                subevent_name = "SET_REPORT_RESPONSE";
                break;
            case 0x0A:
                subevent_name = "GET_PROTOCOL_RESPONSE";
                break;
            case 0x0B:
                subevent_name = "SET_PROTOCOL_RESPONSE";
                break;
            }
            DBG("BTX: Unhandled HID subevent: 0x%02x (%s) (size: %d)\n", subevent, subevent_name, size);
            if (size <= 32)
            {
                DBG("BTX: HID event data: ");
                for (int i = 0; i < size; i++)
                {
                    DBG("%02x ", packet[i]);
                }
                DBG("\n");
            }
            break;
        }
    }
    break;

    default:
        // Log events that might be relevant to gamepad pairing
        // Skip common flow control events and unknown inquiry variations
        if (event_type != 0x6e && event_type != 0x13 && // Skip flow control events
            event_type != 0x12 && event_type != 0x61 && // Skip role change and mode change (now handled)
            event_type != 0x1b && event_type != 0x0b && // Skip max slots and packet type change
            event_type != 0x23 && event_type != 0xdc && // Skip QoS setup and unknown inquiry variant
            event_type != 0xe0 && event_type != 0x73 && // Skip unknown authentication-related event and event 0x73
            event_type != 0xff)                         // Skip vendor-specific events (handled above)
        {
            // DBG("BTX: Unhandled HCI event 0x%02x (size: %d)\n", event_type, size);
            // // Print first few bytes for debugging only for potentially important events
            // if (size <= 16)
            // {
            //     DBG("BTX: Event data: ");
            //     for (int i = 0; i < size; i++)
            //     {
            //         DBG("%02x ", packet[i]);
            //     }
            //     DBG("\n");
            // }
        }
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
    // Enable SSP by default for modern gamepads
    gap_ssp_set_enable(1); // Enable SSP for modern gamepad compatibility

    // This is the GLOBAL default that will be used when IO Capability Request handler doesn't trigger
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);

    // Set authentication requirements for SSP - use dedicated bonding for better gamepad compatibility
    // Many gamepads expect bonding to store the pairing information permanently
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_set_bondable_mode(1);
    DBG("BTX: Bondable mode enabled\n");

    btx_initialized = true;

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
}

bool btx_start_pairing(void)
{
    if (!btx_initialized)
    {
        DBG("BTX: Cannot start pairing - not initialized\n");
        return false;
    }

    DBG("BTX: *** STARTING ACTIVE GAMEPAD SEARCH ***\n");

    // Clear any existing link keys to prevent "PIN or Key Missing" errors
    // This is especially important for Xbox One gamepads and other devices
    // that may have stale bonding information from previous pairing attempts
    gap_delete_all_link_keys();
    DBG("BTX: Cleared all existing link keys to prevent authentication errors\n");

    btx_pairing_mode = true;

    // Make sure we're discoverable and connectable for incoming connections
    gap_discoverable_control(1);
    gap_connectable_control(1);
    DBG("BTX: Enabled discoverable and connectable modes\n");

    // Try a simple inquiry first to see if the command works at all
    DBG("BTX: Attempting inquiry with LAP 0x9E8B33, length 0x08 (10.24s), num_responses 0x00 (unlimited)\n");
    uint8_t result = hci_send_cmd(&hci_inquiry, 0x9E8B33, 0x08, 0x00);
    DBG("BTX: hci_send_cmd returned: %d\n", result);

    // Enable inquiry scan mode explicitly for better gamepad compatibility
    hci_send_cmd(&hci_write_scan_enable, 0x03); // Both inquiry and page scan
    DBG("BTX: Enabled both inquiry and page scan modes\n");

    // Set inquiry scan parameters for better visibility
    // Make inquiry scan more frequent and longer window for better gamepad discovery
    hci_send_cmd(&hci_write_inquiry_scan_activity, 0x1000, 0x0800); // interval=0x1000, window=0x0800
    DBG("BTX: Enhanced inquiry scan parameters for better gamepad discovery\n");

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
}

void btx_reset(void)
{
    // TODO when called will interrupt pairing session
}

#endif /* RP6502_RIA_W */
