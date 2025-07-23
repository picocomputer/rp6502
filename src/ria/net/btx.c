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
    uint16_t acl_handle;             // ACL connection handle
    bool authenticated;              // Whether authentication is complete
    bool hid_attempted;              // Whether we've already attempted HID connection
    uint32_t hid_attempt_time;       // When we attempted HID connection (for timeout)
    bool descriptor_requested;       // Whether we've requested a HID descriptor
    bool mounted_without_descriptor; // Whether we successfully mounted without descriptor

    // Device capability tracking from IO Capability Response
    bool capability_known;           // Whether we've received the device's IO Capability Response
    uint8_t device_io_capability;    // Device's reported IO capability
    uint8_t device_oob_data_present; // Device's reported OOB data status
    uint8_t device_auth_requirement; // Device's reported authentication requirement
} btx_connection_t;

static btx_connection_t btx_connections[PAD_MAX_PLAYERS];
static bool btx_initialized = false;
static bool btx_pairing_mode = false;

// Debug mode: Accept all incoming connections (for troubleshooting 8BitDo issues)
static bool btx_accept_all_incoming = false;

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
    {
        uint8_t state = btstack_event_state_get_state(packet);
        DBG("BTX: BTSTACK_EVENT_STATE: %d\n", state);
        if (state == HCI_STATE_WORKING)
        {
            DBG("BTX: Bluetooth Classic HID Host ready and working!\n");

            // Always re-enable discoverable/connectable when stack becomes ready
            // This is essential because the stack may reset discoverability during initialization
            // gap_discoverable_control(1);
            // gap_connectable_control(1);
            // DBG("BTX: Re-enabled discoverable/connectable in working state\n");
        }
        else
        {
            DBG("BTX: Bluetooth stack state: %d (not working yet)\n", state);
        }
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

    case HCI_EVENT_IO_CAPABILITY_REQUEST:
        hci_event_io_capability_request_get_bd_addr(packet, event_addr);
        DBG("BTX: *** IO CAPABILITY REQUEST *** from %s\n", bd_addr_to_str(event_addr));

        // Check if we have stored device capabilities from a previous IO Capability Response
        int conn_slot = find_connection_by_addr(event_addr);
        uint8_t io_capability;
        uint8_t oob_data_present;
        uint8_t auth_requirement;

        if (conn_slot >= 0 && btx_connections[conn_slot].capability_known)
        {
            // Use device's reported capabilities to configure our response
            io_capability = btx_connections[conn_slot].device_io_capability;
            oob_data_present = btx_connections[conn_slot].device_oob_data_present;
            auth_requirement = btx_connections[conn_slot].device_auth_requirement;

            DBG("BTX: Using stored device capabilities: IO=0x%02x, OOB=%d, Auth=0x%02x\n",
                io_capability, oob_data_present, auth_requirement);
        }
        else
        {
            // Use conservative defaults for first-time pairing
            io_capability = 0x00;    // DisplayOnly (most compatible)
            oob_data_present = 0x00; // No OOB data initially
            auth_requirement = 0x00; // No MITM protection initially

            DBG("BTX: No stored capabilities - using defaults: DisplayOnly (0x00), OOB=No, Auth=0x%02x\n", auth_requirement);
        }

        hci_send_cmd(&hci_io_capability_request_reply, event_addr, io_capability, oob_data_present, auth_requirement);
        break;

    case HCI_EVENT_INQUIRY_RESULT:
    {
        bd_addr_t addr;
        hci_event_inquiry_result_get_bd_addr(packet, addr);
        uint32_t cod = hci_event_inquiry_result_get_class_of_device(packet);

        // DBG("BTX: Inquiry result from %s, CoD: 0x%06lx\n", bd_addr_to_str(addr), (unsigned long)cod);

        // // Check if this looks like a gamepad
        // // Major device class 0x05 = Peripheral, Minor class bits for joystick/gamepad
        // uint8_t major_class = (cod >> 8) & 0x1F;
        // uint8_t minor_class = (cod >> 2) & 0x3F;

        // // Decode Class of Device for better understanding
        // const char *major_desc = "Unknown";
        // switch (major_class)
        // {
        // case 0x00:
        //     major_desc = "Miscellaneous";
        //     break;
        // case 0x01:
        //     major_desc = "Computer";
        //     break;
        // case 0x02:
        //     major_desc = "Phone";
        //     break;
        // case 0x03:
        //     major_desc = "LAN/Network";
        //     break;
        // case 0x04:
        //     major_desc = "Audio/Video";
        //     break;
        // case 0x05:
        //     major_desc = "Peripheral";
        //     break;
        // case 0x06:
        //     major_desc = "Imaging";
        //     break;
        // case 0x07:
        //     major_desc = "Wearable";
        //     break;
        // case 0x08:
        //     major_desc = "Toy";
        //     break;
        // case 0x09:
        //     major_desc = "Health";
        //     break;
        // case 0x1F:
        //     major_desc = "Uncategorized";
        //     break;
        // }

        // DBG("BTX: Device analysis - Major: 0x%02x (%s), Minor: 0x%02x\n", major_class, major_desc, minor_class);

        // // Check for HID service bit (bit 13 in CoD)
        // bool has_hid_service = (cod & (1 << 13)) != 0;
        // DBG("BTX: HID service bit: %s\n", has_hid_service ? "Present" : "Not present");

        // // Debug mode: Accept all devices during inquiry
        // if (btx_accept_all_incoming)
        // {
        //     DBG("BTX: DEBUG MODE: Accepting device %s regardless of classification\n", bd_addr_to_str(addr));
        // }
        // else
        // {
        //     // Only attempt connection to devices that advertise HID service or are likely gamepads
        //     if (!has_hid_service && major_class != 0x05)
        //     {
        //         // Special case: Some gamepads might be classified as "Toy" or "Miscellaneous"
        //         // Also allow Audio/Video devices as some gamepads incorrectly classify themselves
        //         if (major_class != 0x08 && major_class != 0x00 && major_class != 0x04)
        //         {
        //             DBG("BTX: Device %s does not advertise HID service and is not a likely gamepad device\n", bd_addr_to_str(addr));
        //             DBG("BTX: Major class 0x%02x (%s) - skipping connection\n", major_class, major_desc);
        //             DBG("BTX: Continuing inquiry for actual gamepad devices...\n");
        //             break;
        //         }
        //         else
        //         {
        //             DBG("BTX: Device %s might be a gamepad despite Class of Device classification\n", bd_addr_to_str(addr));
        //             DBG("BTX: Major class 0x%02x (%s) - attempting connection anyway\n", major_class, major_desc);
        //         }
        //     }
        // }

        // // Don't try to guess what's a gamepad from Class of Device
        // // Just connect to everything and let pad_mount() determine if it's actually a gamepad
        DBG("BTX: Found device %s (CoD: 0x%06lx) - attempting connection\n", bd_addr_to_str(addr), (unsigned long)cod);
        DBG("BTX: Will determine if it's a gamepad after HID connection is established\n");

        // Try to connect to this device
        hci_send_cmd(&hci_create_connection, addr, 0xCC18, 0x01, 0x00, 0x00, 0x01);
        break;
    }

    case HCI_EVENT_INQUIRY_COMPLETE:
        DBG("BTX: Inquiry complete\n");
        break;

    case 0x22: // HCI_EVENT_INQUIRY_RESULT_WITH_RSSI
    {
        bd_addr_t addr;
        // For inquiry result with RSSI, BD_ADDR is at offset 3-8, similar to basic inquiry
        hci_event_inquiry_result_get_bd_addr(packet, addr);
        uint32_t cod = hci_event_inquiry_result_get_class_of_device(packet);
        int8_t rssi = (int8_t)packet[15]; // RSSI is typically at the end

        DBG("BTX: Inquiry result with RSSI from %s, CoD: 0x%06lx, RSSI: %d dBm\n",
            bd_addr_to_str(addr), (unsigned long)cod, rssi);

        // Check if this looks like a gamepad
        uint8_t major_class = (cod >> 8) & 0x1F;
        uint8_t minor_class = (cod >> 2) & 0x3F;

        // Decode Class of Device for better understanding
        const char *major_desc = "Unknown";
        switch (major_class)
        {
        case 0x00:
            major_desc = "Miscellaneous";
            break;
        case 0x01:
            major_desc = "Computer";
            break;
        case 0x02:
            major_desc = "Phone";
            break;
        case 0x03:
            major_desc = "LAN/Network";
            break;
        case 0x04:
            major_desc = "Audio/Video";
            break;
        case 0x05:
            major_desc = "Peripheral";
            break;
        case 0x06:
            major_desc = "Imaging";
            break;
        case 0x07:
            major_desc = "Wearable";
            break;
        case 0x08:
            major_desc = "Toy";
            break;
        case 0x09:
            major_desc = "Health";
            break;
        case 0x1F:
            major_desc = "Uncategorized";
            break;
        }

        DBG("BTX: Device analysis (RSSI) - Major: 0x%02x (%s), Minor: 0x%02x\n", major_class, major_desc, minor_class);

        // Check for HID service bit (bit 13 in CoD)
        bool has_hid_service = (cod & (1 << 13)) != 0;
        DBG("BTX: HID service bit: %s\n", has_hid_service ? "Present" : "Not present");

        // Debug mode: Accept all devices during inquiry
        if (btx_accept_all_incoming)
        {
            DBG("BTX: DEBUG MODE: Accepting device %s regardless of classification\n", bd_addr_to_str(addr));
        }
        else
        {
            // Only attempt connection to devices that advertise HID service or are likely gamepads
            if (!has_hid_service && major_class != 0x05)
            {
                // Special case: Some gamepads might be classified as "Toy" or "Miscellaneous"
                // Also allow Audio/Video devices as some gamepads incorrectly classify themselves
                if (major_class != 0x08 && major_class != 0x00 && major_class != 0x04)
                {
                    DBG("BTX: Device %s does not advertise HID service and is not a likely gamepad device\n", bd_addr_to_str(addr));
                    DBG("BTX: Major class 0x%02x (%s) - skipping connection\n", major_class, major_desc);
                    DBG("BTX: Continuing inquiry for actual gamepad devices...\n");
                    break;
                }
                else
                {
                    DBG("BTX: Device %s might be a gamepad despite Class of Device classification\n", bd_addr_to_str(addr));
                    DBG("BTX: Major class 0x%02x (%s) - attempting connection anyway\n", major_class, major_desc);
                }
            }
        }

        // Don't try to guess what's a gamepad from Class of Device
        // Just connect to everything and let pad_mount() determine if it's actually a gamepad
        DBG("BTX: Found device %s (CoD: 0x%06lx) via RSSI inquiry - attempting connection\n", bd_addr_to_str(addr), (unsigned long)cod);
        DBG("BTX: Will determine if it's a gamepad after HID connection is established\n");

        // Try to connect to this device
        hci_send_cmd(&hci_create_connection, addr, 0xCC18, 0x01, 0x00, 0x00, 0x01);
        break;
    }

    case 0x2F: // HCI_EVENT_EXTENDED_INQUIRY_RESULT
    {
        bd_addr_t addr;
        // For extended inquiry result, the BD_ADDR is at offset 3-8
        memcpy(addr, packet + 3, BD_ADDR_LEN);
        uint32_t cod = little_endian_read_24(packet, 9); // Class of Device at offset 9-11
        uint8_t rssi = packet[2];                        // RSSI at offset 2

        DBG("BTX: Extended inquiry result from %s, CoD: 0x%06lx, RSSI: %d dBm\n",
            bd_addr_to_str(addr), (unsigned long)cod, (int8_t)rssi);

        // Check if this looks like a gamepad
        uint8_t major_class = (cod >> 8) & 0x1F;
        uint8_t minor_class = (cod >> 2) & 0x3F;

        // Decode Class of Device for better understanding
        const char *major_desc = "Unknown";
        switch (major_class)
        {
        case 0x00:
            major_desc = "Miscellaneous";
            break;
        case 0x01:
            major_desc = "Computer";
            break;
        case 0x02:
            major_desc = "Phone";
            break;
        case 0x03:
            major_desc = "LAN/Network";
            break;
        case 0x04:
            major_desc = "Audio/Video";
            break;
        case 0x05:
            major_desc = "Peripheral";
            break;
        case 0x06:
            major_desc = "Imaging";
            break;
        case 0x07:
            major_desc = "Wearable";
            break;
        case 0x08:
            major_desc = "Toy";
            break;
        case 0x09:
            major_desc = "Health";
            break;
        case 0x1F:
            major_desc = "Uncategorized";
            break;
        }

        DBG("BTX: Extended device analysis - Major: 0x%02x (%s), Minor: 0x%02x\n", major_class, major_desc, minor_class);

        // Check for HID service bit (bit 13 in CoD)
        bool has_hid_service = (cod & (1 << 13)) != 0;
        DBG("BTX: HID service bit: %s\n", has_hid_service ? "Present" : "Not present");

        // Debug mode: Accept all devices during inquiry
        if (btx_accept_all_incoming)
        {
            DBG("BTX: DEBUG MODE: Accepting device %s regardless of classification\n", bd_addr_to_str(addr));
        }
        else
        {
            // Only attempt connection to devices that advertise HID service or are likely gamepads
            if (!has_hid_service && major_class != 0x05)
            {
                // Special case: Some gamepads might be classified as "Toy" or "Miscellaneous"
                // Also allow Audio/Video devices as some gamepads incorrectly classify themselves
                if (major_class != 0x08 && major_class != 0x00 && major_class != 0x04)
                {
                    DBG("BTX: Device %s does not advertise HID service and is not a likely gamepad device\n", bd_addr_to_str(addr));
                    DBG("BTX: Major class 0x%02x (%s) - skipping connection\n", major_class, major_desc);
                    DBG("BTX: Continuing inquiry for actual gamepad devices...\n");
                    break;
                }
                else
                {
                    DBG("BTX: Device %s might be a gamepad despite Class of Device classification\n", bd_addr_to_str(addr));
                    DBG("BTX: Major class 0x%02x (%s) - attempting connection anyway\n", major_class, major_desc);
                }
            }
        }

        // Don't try to guess what's a gamepad from Class of Device
        // Just connect to everything and let pad_mount() determine if it's actually a gamepad
        DBG("BTX: Found device %s (CoD: 0x%06lx) via extended inquiry - attempting connection\n", bd_addr_to_str(addr), (unsigned long)cod);
        DBG("BTX: Will determine if it's a gamepad after HID connection is established\n");

        // Try to connect to this device
        hci_send_cmd(&hci_create_connection, addr, 0xCC18, 0x01, 0x00, 0x00, 0x01);
        break;
    }

    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
        status = hci_event_remote_name_request_complete_get_status(packet);
        hci_event_remote_name_request_complete_get_bd_addr(packet, event_addr);
        if (status == 0)
        {
            char name_buffer[248];
            const char *remote_name = (const char *)hci_event_remote_name_request_complete_get_remote_name(packet);
            int name_len = strlen(remote_name);
            if (name_len > 247)
                name_len = 247;
            memcpy(name_buffer, remote_name, name_len);
            name_buffer[name_len] = 0;
            DBG("BTX: Remote name for %s: '%s'\n", bd_addr_to_str(event_addr), name_buffer);

            // Check if this is an Xbox controller - they need special handling
            bool is_xbox_controller = (strstr(name_buffer, "Xbox") != NULL) ||
                                      (strstr(name_buffer, "XBOX") != NULL);

            if (is_xbox_controller)
            {
                DBG("BTX: *** XBOX CONTROLLER DETECTED *** - using optimized pairing approach\n");
                DBG("BTX: Xbox controllers often disconnect quickly if authentication isn't handled properly\n");

                // Find the connection for this device and trigger authentication immediately
                int slot = find_connection_by_addr(event_addr);
                if (slot >= 0)
                {
                    DBG("BTX: Triggering immediate authentication for Xbox controller at slot %d\n", slot);
                    hci_send_cmd(&hci_authentication_requested, btx_connections[slot].acl_handle);
                }
                else
                {
                    DBG("BTX: Could not find connection slot for Xbox controller\n");
                }
            }
        }
        else
        {
            DBG("BTX: Remote name request failed for %s, status: 0x%02x\n", bd_addr_to_str(event_addr), status);
        }
        break;

    case HCI_EVENT_CONNECTION_REQUEST:
        hci_event_connection_request_get_bd_addr(packet, event_addr);
        uint32_t class_of_device = hci_event_connection_request_get_class_of_device(packet);
        DBG("BTX: *** INCOMING CONNECTION REQUEST! ***\n");
        DBG("BTX: Connection request from %s, CoD: 0x%06lx\n", bd_addr_to_str(event_addr), (unsigned long)class_of_device);

        // Decode the incoming device's Class of Device
        uint8_t major_class = (class_of_device >> 8) & 0x1F;
        uint8_t minor_class = (class_of_device >> 2) & 0x3F;
        const char *major_desc = "Unknown";
        switch (major_class)
        {
        case 0x00:
            major_desc = "Miscellaneous";
            break;
        case 0x01:
            major_desc = "Computer";
            break;
        case 0x02:
            major_desc = "Phone";
            break;
        case 0x03:
            major_desc = "LAN/Network";
            break;
        case 0x04:
            major_desc = "Audio/Video";
            break;
        case 0x05:
            major_desc = "Peripheral";
            break;
        case 0x06:
            major_desc = "Imaging";
            break;
        case 0x07:
            major_desc = "Wearable";
            break;
        case 0x08:
            major_desc = "Toy";
            break;
        case 0x09:
            major_desc = "Health";
            break;
        case 0x1F:
            major_desc = "Uncategorized";
            break;
        }

        bool has_hid_service = (class_of_device & (1 << 13)) != 0;
        DBG("BTX: Incoming device - Major: 0x%02x (%s), Minor: 0x%02x, HID service: %s\n",
            major_class, major_desc, minor_class, has_hid_service ? "Yes" : "No");

        // Debug mode: Accept all incoming connections
        if (btx_accept_all_incoming)
        {
            DBG("BTX: DEBUG MODE: Accepting ALL incoming connections (btx_accept_all_incoming=true)\n");
            DBG("BTX: This helps identify 8BitDo or other problematic gamepad classifications\n");
        }
        else
        {
            // Only accept connections from devices that likely have gamepads
            if (!has_hid_service && major_class != 0x05)
            {
                // Special case: Some gamepads might be classified as "Toy" or "Miscellaneous"
                // Also allow Audio/Video devices as some gamepads incorrectly classify themselves
                if (major_class != 0x08 && major_class != 0x00 && major_class != 0x04)
                {
                    DBG("BTX: Rejecting connection - device does not advertise HID service and is not a likely gamepad\n");
                    DBG("BTX: Major class 0x%02x (%s) appears to be a computer/phone, not a gamepad\n", major_class, major_desc);

                    // For debugging: Let's see if we're rejecting 8BitDo gamepads by mistake
                    DBG("BTX: If this was an 8BitDo or similar gamepad, it may have wrong classification\n");
                    DBG("BTX: Consider temporarily accepting all incoming connections for testing\n");

                    hci_send_cmd(&hci_reject_connection_request, event_addr, ERROR_CODE_CONNECTION_REJECTED_DUE_TO_LIMITED_RESOURCES);
                    break;
                }
                else
                {
                    DBG("BTX: Device might be a gamepad despite Class of Device - accepting connection\n");
                    DBG("BTX: Major class 0x%02x (%s) could be a gamepad\n", major_class, major_desc);
                }
            }
            else
            {
                DBG("BTX: Device has proper HID service or Peripheral classification - accepting\n");
            }
        }

        // Accept connection requests from devices that could be gamepads
        // Let pad_mount() determine later if it's actually a gamepad
        DBG("BTX: Accepting connection from potential gamepad device\n");
        hci_send_cmd(&hci_accept_connection_request, event_addr, HCI_ROLE_SLAVE);
        DBG("BTX: Accepting connection as slave device\n");
        break;

    case HCI_EVENT_CONNECTION_COMPLETE: // TODO this should be HID_SUBEVENT_INCOMING_CONNECTION
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
                btx_connections[slot].authenticated = false;
                btx_connections[slot].hid_attempted = false;
                btx_connections[slot].hid_cid = 0;
                btx_connections[slot].hid_attempt_time = 0;

                // Initialize device capability tracking
                btx_connections[slot].capability_known = false;
                btx_connections[slot].device_io_capability = 0;
                btx_connections[slot].device_oob_data_present = 0;
                btx_connections[slot].device_auth_requirement = 0;

                DBG("BTX: Tracking ACL connection at slot %d - waiting for authentication before HID attempt\n", slot);
                DBG("BTX: Device: %s, Handle: 0x%04x\n", bd_addr_to_str(event_addr), handle);

                // Request remote name to help identify the device
                DBG("BTX: Requesting remote name for device identification...\n");
                hci_send_cmd(&hci_remote_name_request, event_addr, 0x01, 0x00, 0x00);

                // Some gamepads need us to initiate authentication explicitly
                // Wait a bit first to let the connection stabilize
                DBG("BTX: Will attempt authentication in 100ms to allow connection to stabilize\n");

                // hci_send_cmd(&hci_authentication_requested, btx_connections[slot].acl_handle);

                //////////////////////////////////////
            }
            else
            {
                DBG("BTX: No free slots to track connection - this may cause issues\n");
            }

            // Don't attempt HID connection immediately - wait for authentication to complete
            DBG("BTX: Waiting for authentication to complete before attempting HID connection\n");
            DBG("BTX: If device disconnects quickly, it may not like our connection parameters\n");
        }
        else
        {
            const char *conn_error = "Unknown connection error";
            switch (status)
            {
            case 0x04:
                conn_error = "Page Timeout";
                break;
            case 0x05:
                conn_error = "Authentication Failure";
                break;
            case 0x08:
                conn_error = "Connection Timeout";
                break;
            case 0x0E:
                conn_error = "Connection Rejected - Limited Resources";
                break;
            case 0x0F:
                conn_error = "Connection Rejected - Security Reasons";
                break;
            case 0x11:
                conn_error = "Connection Accept Timeout Exceeded";
                break;
            case 0x16:
                conn_error = "Connection Terminated by Local Host";
                break;
            }
            DBG("BTX: ACL connection failed with %s, status: 0x%02x (%s)\n",
                bd_addr_to_str(event_addr), status, conn_error);

            // Provide troubleshooting advice based on error
            if (status == 0x04)
            {
                DBG("BTX: Page Timeout - device may not be responding or is too far away\n");
            }
            else if (status == 0x0F)
            {
                DBG("BTX: Security rejection - device may require different security settings\n");
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
            }

            DBG("BTX: ACL disconnection complete, handle: 0x%04x, reason: 0x%02x, device: %s\n",
                handle, reason, device_addr);

            // Decode the disconnection reason for better debugging
            const char *reason_desc = "Unknown reason";
            switch (reason)
            {
            case 0x05:
                reason_desc = "Authentication Failure";
                break;
            case 0x06:
                reason_desc = "PIN or Key Missing";
                break;
            case 0x08:
                reason_desc = "Connection Timeout";
                break;
            case 0x0E:
                reason_desc = "Connection Rejected - Limited Resources";
                break;
            case 0x0F:
                reason_desc = "Connection Rejected - Security Reasons";
                break;
            case 0x13:
                reason_desc = "Connection Terminated - User Ended Connection";
                break;
            case 0x16:
                reason_desc = "Connection Terminated by Local Host";
                break;
            case 0x22:
                reason_desc = "LMP Response Timeout";
                break;
            case 0x28:
                reason_desc = "Instant Passed";
                break;
            case 0x2A:
                reason_desc = "Parameter Out of Mandatory Range";
                break;
            case 0x3D:
                reason_desc = "Connection Rejected - No Suitable Channel Found";
                break;
            }
            DBG("BTX: Disconnection reason: %s (0x%02x)\n", reason_desc, reason);

            // Provide specific troubleshooting advice based on disconnection reason
            if (reason == 0x05)
            {
                DBG("BTX: Authentication failure - device may need different pairing approach\n");
                DBG("BTX: Try toggling SSP mode or check if device needs factory reset\n");
            }
            else if (reason == 0x08)
            {
                DBG("BTX: Connection timeout - device may not be responding properly\n");
                DBG("BTX: Ensure device is in correct pairing mode and try again\n");
            }
            else if (reason == 0x13)
            {
                DBG("BTX: Device ended the connection - this is normal behavior\n");
                DBG("BTX: Device may have finished what it needed to do\n");

                // Special advice for Xbox controllers that disconnect with reason 0x13
                if (slot >= 0)
                {
                    // Check if this was an Xbox controller by looking at any cached name info
                    // Since we don't store the name, we'll give general Xbox advice
                    DBG("BTX: If this was an Xbox controller, this is a common issue:\n");
                    DBG("BTX:   1. Xbox controllers are very timing-sensitive during pairing\n");
                    DBG("BTX:   2. They often disconnect if authentication doesn't start quickly enough\n");
                    DBG("BTX:   3. Try putting the controller in pairing mode again immediately\n");
                    DBG("BTX:   4. Some Xbox controllers require being connected to Windows first\n");
                    DBG("BTX:   5. Try holding the Xbox button + pairing button for 6+ seconds to reset\n");
                    DBG("BTX:   6. Modern Xbox controllers may need SSP instead of PIN pairing\n");
                }
            }
            else if (reason == 0x22)
            {
                DBG("BTX: LMP Response Timeout - device may not support our link mode\n");
                DBG("BTX: Try different link policy settings or SSP mode\n");
            }
            else if (reason == 0x0F)
            {
                DBG("BTX: Security rejection - device requires different security level\n");
                DBG("BTX: Try enabling SSP or check authentication requirements\n");
            }

            // Clean up connection tracking
            if (slot >= 0)
            {
                DBG("BTX: Cleaning up connection tracking for slot %d\n", slot);
                if (btx_connections[slot].hid_cid != 0)
                {
                    // HID connection should be cleaned up by HID_SUBEVENT_CONNECTION_CLOSED
                    // but make sure gamepad is unmounted
                    pad_umount(btx_slot_to_pad_idx(slot));
                }
                btx_connections[slot].active = false;
                btx_connections[slot].hid_cid = 0;
                btx_connections[slot].authenticated = false;
                btx_connections[slot].hid_attempted = false;
                btx_connections[slot].hid_attempt_time = 0;

                // Clear device capability tracking
                btx_connections[slot].capability_known = false;
                btx_connections[slot].device_io_capability = 0;
                btx_connections[slot].device_oob_data_present = 0;
                btx_connections[slot].device_auth_requirement = 0;
            }
        }
        else
        {
            DBG("BTX: Disconnection complete event failed, status: 0x%02x\n", status);
        }
        break;

    case HCI_EVENT_LINK_KEY_REQUEST:
        hci_event_link_key_request_get_bd_addr(packet, event_addr);

        // Check if this is an Xbox controller for special handling
        bool is_xbox = (event_addr[0] == 0x9C) || (event_addr[0] == 0x58) || (event_addr[0] == 0x88);

        if (is_xbox)
        {
            DBG("BTX: Link key request from XBOX CONTROLLER %s - forcing fresh authentication\n", bd_addr_to_str(event_addr));
            DBG("BTX: This should trigger IO Capability Request and SSP authentication\n");
        }
        else
        {
            DBG("BTX: Link key request from %s - sending negative reply to force PIN authentication\n", bd_addr_to_str(event_addr));
            DBG("BTX: This should trigger a PIN code request next\n");
        }

        // Always send negative reply to force fresh authentication (no stored keys)
        hci_send_cmd(&hci_link_key_request_negative_reply, event_addr);
        break;

    case HCI_EVENT_LINK_KEY_NOTIFICATION:
        DBG("BTX: Link key notification received\n");
        break;

    case HCI_EVENT_ROLE_CHANGE:
    {
        status = packet[2];
        hci_event_role_change_get_bd_addr(packet, event_addr);
        uint8_t new_role = packet[9];
        if (status == 0)
        {
            DBG("BTX: Role change successful for %s, new role: %s\n",
                bd_addr_to_str(event_addr),
                new_role == HCI_ROLE_MASTER ? "MASTER" : "SLAVE");
        }
        else
        {
            DBG("BTX: Role change failed for %s, status: 0x%02x\n", bd_addr_to_str(event_addr), status);
        }
        break;
    }

    case HCI_EVENT_MODE_CHANGE:
    {
        status = packet[2];
        uint16_t handle = little_endian_read_16(packet, 3);
        uint8_t current_mode = packet[5];
        uint16_t interval = little_endian_read_16(packet, 6);
        if (status == 0)
        {
            const char *mode_str = "UNKNOWN";
            switch (current_mode)
            {
            case 0x00:
                mode_str = "ACTIVE";
                break;
            case 0x01:
                mode_str = "HOLD";
                break;
            case 0x02:
                mode_str = "SNIFF";
                break;
            case 0x03:
                mode_str = "PARK";
                break;
            }
            DBG("BTX: Mode change to %s for handle 0x%04x, interval: %d\n", mode_str, handle, interval);
        }
        else
        {
            DBG("BTX: Mode change failed for handle 0x%04x, status: 0x%02x\n", handle, status);
        }
        break;
    }

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
            else
            {
                DBG("BTX: WARNING: Could not find ACL connection slot for HID connection from %s\n", bd_addr_to_str(event_addr));
            }

            // Always accept incoming HID connections when discoverable (BTStack pattern)
            hid_host_accept_connection(hid_cid, HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT);
            DBG("BTX: Accepting incoming HID connection with report protocol mode\n");
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
                            btx_connections[i].mounted_without_descriptor = false; // Mounted with descriptor
                        }
                        else
                        {
                            DBG("BTX: Device at slot %d is NOT a gamepad - pad_mount returned false\n", i);
                            DBG("BTX: Disconnecting non-gamepad device\n");

                            // Clean up the connection since it's not a gamepad
                            hid_host_disconnect(hid_cid);
                            btx_connections[i].active = false;
                        }
                    }
                    else
                    {
                        DBG("BTX: Failed to get HID descriptor for device at slot %d, status: 0x%02x\n", i, status);

                        // Device never sent descriptor - try fallback mounting with synthesized USB IDs
                        DBG("BTX: Trying immediate mount without descriptor\n");
                        bool mounted = pad_mount(btx_slot_to_pad_idx(i), NULL, 0, 0, 0, 0);
                        if (mounted)
                        {
                            DBG("BTX: *** GAMEPAD CONFIRMED! *** Mounted with fallback method at slot %d\n", i);
                            DBG("BTX: This gamepad works without sending a HID descriptor\n");
                            btx_connections[i].mounted_without_descriptor = true;
                        }
                        else
                        {
                            DBG("BTX: Device at slot %d is NOT a gamepad - fallback pad_mount returned false\n", i);
                            DBG("BTX: Disconnecting non-gamepad device\n");

                            // Clean up the connection since it's not a gamepad
                            hid_host_disconnect(hid_cid);
                            btx_connections[i].active = false;
                        }
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

        default:
            DBG("BTX: Unknown HID subevent: 0x%02x (size: %d)\n", subevent, size);
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

    case BTSTACK_EVENT_SCAN_MODE_CHANGED:
    {
        uint8_t discoverable = packet[2];
        uint8_t connectable = packet[3];
        DBG("BTX: Discoverable mode %d, connectable %d\n", discoverable, connectable);
    }
    break;

    case HCI_EVENT_COMMAND_STATUS:
    {
        uint8_t status = packet[2];
        uint16_t opcode = little_endian_read_16(packet, 4);

        if (opcode == 0x0401)
        { // HCI_Inquiry
            if (status == 0)
            {
                DBG("BTX: Inquiry command accepted and running\n");
            }
            else
            {
                DBG("BTX: Inquiry command rejected with status: 0x%02x\n", status);
            }
        }
        break;
    }

    case HCI_EVENT_IO_CAPABILITY_RESPONSE:
    {
        hci_event_io_capability_response_get_bd_addr(packet, event_addr);
        uint8_t io_capability = hci_event_io_capability_response_get_io_capability(packet);
        uint8_t oob_data_present = hci_event_io_capability_response_get_oob_data_present(packet);
        uint8_t auth_requirement = hci_event_io_capability_response_get_authentication_requirements(packet);

        // Store the device's actual requirements for future reference
        int conn_slot = find_connection_by_addr(event_addr);
        if (conn_slot >= 0)
        {
            btx_connections[conn_slot].capability_known = true;
            btx_connections[conn_slot].device_io_capability = io_capability;
            btx_connections[conn_slot].device_oob_data_present = oob_data_present;
            btx_connections[conn_slot].device_auth_requirement = auth_requirement;
            DBG("BTX: Stored device capabilities for slot %d: IO=0x%02x, OOB=%d, Auth=0x%02x\n",
                conn_slot, io_capability, oob_data_present, auth_requirement);
        }
        else
        {
            DBG("BTX: Could not find connection slot to store device capabilities\n");
        }

        break;
    }

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

    // Set IO capabilities for SSP - NoInputNoOutput is optimal for Xbox controllers
    // This is the GLOBAL default that will be used when IO Capability Request handler doesn't trigger
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    DBG("BTX: SSP enabled with NoInputNoOutput capability (global default for Xbox compatibility)\n");

    // Set authentication requirements for SSP - no MITM for Xbox controllers
    // Xbox controllers are very strict about these parameters
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_NO_BONDING);
    DBG("BTX: SSP authentication set to no MITM protection (global default for Xbox compatibility)\n"); // Set bondable mode for both SSP and legacy pairing
    gap_set_bondable_mode(1);
    DBG("BTX: Bondable mode enabled\n");

    // // Make discoverable to allow HID devices to initiate connection (BTStack pattern)
    // // This is ESSENTIAL for gamepad pairing - they need to find and connect to us
    // gap_discoverable_control(1);
    // gap_connectable_control(1);

    // // Enable inquiry scan mode explicitly for better gamepad compatibility
    // hci_send_cmd(&hci_write_scan_enable, 0x03); // Both inquiry and page scan
    // DBG("BTX: Enabled both inquiry and page scan modes\n");

    // // Set inquiry scan parameters for better visibility
    // // Make inquiry scan more frequent and longer window for better gamepad discovery
    // hci_send_cmd(&hci_write_inquiry_scan_activity, 0x1000, 0x0800); // interval=0x1000, window=0x0800
    // DBG("BTX: Enhanced inquiry scan parameters for better gamepad discovery\n");

    btx_initialized = true;

    // Enable debug mode for 8BitDo troubleshooting
    btx_accept_all_incoming = true;
    DBG("BTX: DEBUG MODE ENABLED - Accepting all incoming connections\n");
    DBG("BTX: This helps with 8BitDo and other gamepads that may have unusual classifications\n");

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

    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Static arrays to track connection timing and authentication attempts
    static uint32_t connection_start_time[PAD_MAX_PLAYERS] = {0};
    static bool auth_attempted[PAD_MAX_PLAYERS] = {false};

    // Check for connections that might need authentication help
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
    {
        if (btx_connections[i].active)
        {
            // Initialize timing for new connections
            if (connection_start_time[i] == 0)
            {
                connection_start_time[i] = now;
                auth_attempted[i] = false;
            }

            // If we've had an ACL connection for more than 3 seconds without authentication
            // and we haven't already tried manual authentication
            if (!btx_connections[i].authenticated &&
                !auth_attempted[i] &&
                (now - connection_start_time[i]) > 3000)
            {

                DBG("BTX: Connection at slot %d has been waiting for authentication for >3s\n", i);
                DBG("BTX: Device: %s, Handle: 0x%04x\n",
                    bd_addr_to_str(btx_connections[i].remote_addr),
                    btx_connections[i].acl_handle);
                DBG("BTX: Attempting to trigger authentication manually (one time only)...\n");

                // Try to trigger authentication manually - but only once
                hci_send_cmd(&hci_authentication_requested, btx_connections[i].acl_handle);
                auth_attempted[i] = true;
            }
        }
        else if (connection_start_time[i] != 0)
        {
            // Reset tracking when connection becomes inactive
            connection_start_time[i] = 0;
            auth_attempted[i] = false;
        }
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

    DBG("BTX: Bluetooth Classic HID gamepad support active\n");
    DBG("BTX: Pairing mode: %s\n", btx_pairing_mode ? "ON" : "OFF");
    DBG("BTX: Debug mode (accept all incoming): %s\n", btx_accept_all_incoming ? "ENABLED" : "DISABLED");

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
