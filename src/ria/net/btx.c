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
    uint16_t acl_handle; // ACL connection handle
    bool authenticated; // Whether authentication is complete
    bool hid_attempted; // Whether we've already attempted HID connection
    uint32_t hid_attempt_time; // When we attempted HID connection (for timeout)
    bool descriptor_requested; // Whether we've requested a HID descriptor
    bool mounted_without_descriptor; // Whether we successfully mounted without descriptor

    // Device capability tracking from IO Capability Response
    bool capability_known; // Whether we've received the device's IO Capability Response
    uint8_t device_io_capability; // Device's reported IO capability
    uint8_t device_oob_data_present; // Device's reported OOB data status
    uint8_t device_auth_requirement; // Device's reported authentication requirement
} btx_connection_t;

static btx_connection_t btx_connections[PAD_MAX_PLAYERS];
static bool btx_initialized = false;
static bool btx_pairing_mode = false;

// Debug mode: Accept all incoming connections (for troubleshooting 8BitDo issues)
static bool btx_accept_all_incoming = false;

// PIN retry logic for authentication failures
typedef struct {
    bd_addr_t addr;
    int pin_attempt;
    bool active;
} pin_retry_t;

static pin_retry_t pin_retries[PAD_MAX_PLAYERS];
static const char* common_pins[] = {"0000", "1234", "1111", "0001", NULL};

// BTStack state - Classic HID Host
static btstack_packet_callback_registration_t hci_event_callback_registration;

// Storage for HID descriptors - Classic only
static uint8_t hid_descriptor_storage[500]; // HID descriptor storage

// Forward declarations
static void btx_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static int find_pin_retry_slot(bd_addr_t addr);
// static void clear_pin_retry_slot(bd_addr_t addr); // Currently unused
static int find_connection_by_handle(uint16_t handle);
static int find_connection_by_addr(bd_addr_t addr);
static void attempt_hid_connection(int slot);
static void btx_synthesize_usb_ids(bd_addr_t addr, uint16_t *vid, uint16_t *pid);

/**
 * @brief Synthesize fake USB VID/PID from Bluetooth device address
 *
 * This function creates synthetic USB Vendor ID and Product ID values from the Bluetooth
 * device address, similar to how Linux HID_BLUETOOTH_DEVICE works in reverse.
 * This allows pad_mount() to use device-specific descriptors for Xbox controllers
 * and other gamepads that don't send HID descriptors.
 *
 * @param addr Bluetooth device address (6 bytes)
 * @param vid Pointer to store synthesized Vendor ID
 * @param pid Pointer to store synthesized Product ID
 */
static void btx_synthesize_usb_ids(bd_addr_t addr, uint16_t *vid, uint16_t *pid)
{
    // Default to generic values
    *vid = 0x1234; // Generic "Bluetooth HID" vendor
    *pid = 0x5678; // Generic gamepad

    // Check for known Xbox controller OUI patterns
    // Microsoft uses specific OUI ranges for Xbox controllers
    if (addr[0] == 0x9C && addr[1] == 0xAA) {
        // Xbox One controllers often use this OUI range
        *vid = 0x045E; // Microsoft Corporation
        *pid = 0x02FD; // Xbox One S Controller (common PID)
        DBG("BTX: Detected Xbox One controller pattern - using Microsoft VID/PID\n");
    } else if (addr[0] == 0x58 && addr[1] == 0xFC) {
        // Some Xbox controllers use this range
        *vid = 0x045E; // Microsoft Corporation
        *pid = 0x02EA; // Xbox One Controller
        DBG("BTX: Detected Xbox controller pattern - using Microsoft VID/PID\n");
    } else if (addr[0] == 0x88 && addr[1] == 0xC6) {
        // Another Xbox controller OUI
        *vid = 0x045E; // Microsoft Corporation
        *pid = 0x0719; // Xbox 360 Wireless Receiver
        DBG("BTX: Detected Xbox 360 controller pattern - using Microsoft VID/PID\n");
    } else if (addr[0] == 0x7C && addr[1] == 0xB9) {
        // Some 8BitDo controllers use this OUI
        *vid = 0x2DC8; // 8BitDo
        *pid = 0x6001; // SN30 Pro
        DBG("BTX: Detected 8BitDo controller pattern - using 8BitDo VID/PID\n");
    } else if (addr[0] == 0x00 && addr[1] == 0x1F) {
        // Sony DualShock controllers sometimes use this range
        *vid = 0x054C; // Sony Interactive Entertainment
        *pid = 0x05C4; // DualShock 4 (common)
        DBG("BTX: Detected Sony controller pattern - using Sony VID/PID\n");
    } else {
        // Create a unique PID from the last 3 bytes of the Bluetooth address
        // This ensures consistent IDs for the same device across reconnections
        *pid = 0x8000 | ((addr[3] << 8) | (addr[4] ^ addr[5]));
        DBG("BTX: Unknown controller pattern - using synthesized VID/PID\n");
    }

    DBG("BTX: BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x -> VID=0x%04X, PID=0x%04X\n",
        addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], *vid, *pid);
}

static int find_pin_retry_slot(bd_addr_t addr)
{
    // First try to find existing slot for this address
    for (int i = 0; i < PAD_MAX_PLAYERS; i++) {
        if (pin_retries[i].active && memcmp(pin_retries[i].addr, addr, BD_ADDR_LEN) == 0) {
            return i;
        }
    }

    // Find empty slot
    for (int i = 0; i < PAD_MAX_PLAYERS; i++) {
        if (!pin_retries[i].active) {
            memcpy(pin_retries[i].addr, addr, BD_ADDR_LEN);
            pin_retries[i].pin_attempt = 0;
            pin_retries[i].active = true;
            return i;
        }
    }

    return -1; // No slot available
}

// Currently unused - keeping for potential future use
/*
static void clear_pin_retry_slot(bd_addr_t addr)
{
    for (int i = 0; i < PAD_MAX_PLAYERS; i++) {
        if (pin_retries[i].active && memcmp(pin_retries[i].addr, addr, BD_ADDR_LEN) == 0) {
            pin_retries[i].active = false;
            break;
        }
    }
}
*/

static int find_connection_by_handle(uint16_t handle)
{
    for (int i = 0; i < PAD_MAX_PLAYERS; i++) {
        if (btx_connections[i].active && btx_connections[i].acl_handle == handle) {
            return i;
        }
    }
    return -1;
}

static int find_connection_by_addr(bd_addr_t addr)
{
    for (int i = 0; i < PAD_MAX_PLAYERS; i++) {
        if (btx_connections[i].active && memcmp(btx_connections[i].remote_addr, addr, BD_ADDR_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

static void attempt_hid_connection(int slot)
{
    if (slot < 0 || slot >= PAD_MAX_PLAYERS || !btx_connections[slot].active) {
        return;
    }

    if (btx_connections[slot].hid_attempted) {
        DBG("BTX: HID connection already attempted for slot %d\n", slot);
        return;
    }

    btx_connections[slot].hid_attempted = true;
    btx_connections[slot].hid_attempt_time = to_ms_since_boot(get_absolute_time());

    DBG("BTX: Attempting HID connection to authenticated device at slot %d (%s)\n",
        slot, bd_addr_to_str(btx_connections[slot].remote_addr));

    // Add a small delay before attempting HID connection
    // Some devices need time after encryption is established
    DBG("BTX: Waiting briefly for device to be ready for HID connection...\n");

    uint16_t hid_cid;
    uint8_t hid_status = hid_host_connect(btx_connections[slot].remote_addr,
                                          HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT,
                                          &hid_cid);
    if (hid_status == ERROR_CODE_SUCCESS) {
        DBG("BTX: HID connection request sent successfully for authenticated device, CID: 0x%04x\n", hid_cid);
        DBG("BTX: Now waiting for HID_SUBEVENT_CONNECTION_OPENED event...\n");
        btx_connections[slot].hid_cid = hid_cid;
    } else {
        DBG("BTX: Failed to initiate HID connection for authenticated device, status: 0x%02x\n", hid_status);
        const char *error_desc = "Unknown error";
        switch (hid_status) {
            case 0x02: error_desc = "Unknown Connection Identifier"; break;
            case 0x04: error_desc = "Page Timeout"; break;
            case 0x05: error_desc = "Authentication Failure"; break;
            case 0x08: error_desc = "Connection Timeout"; break;
            case 0x0C: error_desc = "Command Disallowed"; break;
            case 0x0D: error_desc = "Connection Rejected - Limited Resources"; break;
            case 0x0E: error_desc = "Connection Rejected - Security Reasons"; break;
        }
        DBG("BTX: HID connection error: %s (0x%02x)\n", error_desc, hid_status);
        btx_connections[slot].hid_attempted = false; // Allow retry
        btx_connections[slot].hid_attempt_time = 0;
    }
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
            // who the fucks says so? don't do this bullshit
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

        // Get or create PIN retry slot for this device
        int slot = find_pin_retry_slot(event_addr);
        const char *pin = "0000"; // Default fallback

        if (slot >= 0 && common_pins[pin_retries[slot].pin_attempt] != NULL) {
            pin = common_pins[pin_retries[slot].pin_attempt];
            DBG("BTX: PIN code request from %s, trying PIN '%s' (attempt %d)\n",
                bd_addr_to_str(event_addr), pin, pin_retries[slot].pin_attempt + 1);

            // Advance to next PIN for next attempt (in case this one fails)
            pin_retries[slot].pin_attempt++;
        } else {
            DBG("BTX: PIN code request from %s, using default PIN '%s'\n", bd_addr_to_str(event_addr), pin);
        }

        DBG("BTX: Available PINs for retry: 0000, 1234, 1111, 0001\n");
        DBG("BTX: If authentication fails, device should retry with next PIN automatically\n");

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

        if (conn_slot >= 0 && btx_connections[conn_slot].capability_known) {
            // Use device's reported capabilities to configure our response
            io_capability = btx_connections[conn_slot].device_io_capability;
            oob_data_present = btx_connections[conn_slot].device_oob_data_present;
            auth_requirement = btx_connections[conn_slot].device_auth_requirement;

            DBG("BTX: Using stored device capabilities: IO=0x%02x, OOB=%d, Auth=0x%02x\n",
                io_capability, oob_data_present, auth_requirement);
            DBG("BTX: Matching device's requirements to improve authentication success\n");
        } else {
            // Use conservative defaults for first-time pairing
            io_capability = 0x00;      // DisplayOnly (most compatible)
            oob_data_present = 0x00;   // No OOB data initially
            auth_requirement = 0x00;   // No MITM protection initially

            DBG("BTX: No stored capabilities - using defaults: DisplayOnly (0x00), OOB=No, Auth=0x%02x\n", auth_requirement);
            DBG("BTX: Will learn device requirements from its IO Capability Response\n");
        }

        hci_send_cmd(&hci_io_capability_request_reply, event_addr, io_capability, oob_data_present, auth_requirement);

        const char *io_cap_str = "Unknown";
        switch (io_capability) {
            case 0x00: io_cap_str = "DisplayOnly"; break;
            case 0x01: io_cap_str = "DisplayYesNo"; break;
            case 0x02: io_cap_str = "KeyboardOnly"; break;
            case 0x03: io_cap_str = "NoInputNoOutput"; break;
        }

        DBG("BTX: Sent IO Capability Reply: %s (0x%02x), OOB=%s, Auth=0x%02x\n",
            io_cap_str, io_capability, oob_data_present ? "Yes" : "No", auth_requirement);
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

        DBG("BTX: Device analysis - Major: 0x%02x (%s), Minor: 0x%02x\n", major_class, major_desc, minor_class);

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

            if (is_xbox_controller) {
                DBG("BTX: *** XBOX CONTROLLER DETECTED *** - using optimized pairing approach\n");
                DBG("BTX: Xbox controllers often disconnect quickly if authentication isn't handled properly\n");

                // Find the connection for this device and trigger authentication immediately
                int slot = find_connection_by_addr(event_addr);
                if (slot >= 0) {
                    DBG("BTX: Triggering immediate authentication for Xbox controller at slot %d\n", slot);
                    hci_send_cmd(&hci_authentication_requested, btx_connections[slot].acl_handle);
                } else {
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

    case HCI_EVENT_CONNECTION_COMPLETE:
        status = hci_event_connection_complete_get_status(packet);
        hci_event_connection_complete_get_bd_addr(packet, event_addr);
        if (status == 0)
        {
            uint16_t handle = hci_event_connection_complete_get_connection_handle(packet);
            DBG("BTX: ACL connection established with %s, handle: 0x%04x\n", bd_addr_to_str(event_addr), handle);

            // Find an empty slot to track this connection
            int slot = -1;
            for (int i = 0; i < PAD_MAX_PLAYERS; i++) {
                if (!btx_connections[i].active) {
                    slot = i;
                    break;
                }
            }

            if (slot >= 0) {
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

            } else {
                DBG("BTX: No free slots to track connection - this may cause issues\n");
            }

            // Don't attempt HID connection immediately - wait for authentication to complete
            DBG("BTX: Waiting for authentication to complete before attempting HID connection\n");
            DBG("BTX: If device disconnects quickly, it may not like our connection parameters\n");
        }
        else
        {
            const char *conn_error = "Unknown connection error";
            switch (status) {
                case 0x04: conn_error = "Page Timeout"; break;
                case 0x05: conn_error = "Authentication Failure"; break;
                case 0x08: conn_error = "Connection Timeout"; break;
                case 0x0E: conn_error = "Connection Rejected - Limited Resources"; break;
                case 0x0F: conn_error = "Connection Rejected - Security Reasons"; break;
                case 0x11: conn_error = "Connection Accept Timeout Exceeded"; break;
                case 0x16: conn_error = "Connection Terminated by Local Host"; break;
            }
            DBG("BTX: ACL connection failed with %s, status: 0x%02x (%s)\n",
                bd_addr_to_str(event_addr), status, conn_error);

            // Provide troubleshooting advice based on error
            if (status == 0x04) {
                DBG("BTX: Page Timeout - device may not be responding or is too far away\n");
            } else if (status == 0x0F) {
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
            if (slot >= 0) {
                device_addr = bd_addr_to_str(btx_connections[slot].remote_addr);
            }

            DBG("BTX: ACL disconnection complete, handle: 0x%04x, reason: 0x%02x, device: %s\n",
                handle, reason, device_addr);

            // Decode the disconnection reason for better debugging
            const char *reason_desc = "Unknown reason";
            switch (reason) {
                case 0x05: reason_desc = "Authentication Failure"; break;
                case 0x06: reason_desc = "PIN or Key Missing"; break;
                case 0x08: reason_desc = "Connection Timeout"; break;
                case 0x0E: reason_desc = "Connection Rejected - Limited Resources"; break;
                case 0x0F: reason_desc = "Connection Rejected - Security Reasons"; break;
                case 0x13: reason_desc = "Connection Terminated - User Ended Connection"; break;
                case 0x16: reason_desc = "Connection Terminated by Local Host"; break;
                case 0x22: reason_desc = "LMP Response Timeout"; break;
                case 0x28: reason_desc = "Instant Passed"; break;
                case 0x2A: reason_desc = "Parameter Out of Mandatory Range"; break;
                case 0x3D: reason_desc = "Connection Rejected - No Suitable Channel Found"; break;
            }
            DBG("BTX: Disconnection reason: %s (0x%02x)\n", reason_desc, reason);

            // Provide specific troubleshooting advice based on disconnection reason
            if (reason == 0x05) {
                DBG("BTX: Authentication failure - device may need different pairing approach\n");
                DBG("BTX: Try toggling SSP mode or check if device needs factory reset\n");
            } else if (reason == 0x08) {
                DBG("BTX: Connection timeout - device may not be responding properly\n");
                DBG("BTX: Ensure device is in correct pairing mode and try again\n");
            } else if (reason == 0x13) {
                DBG("BTX: Device ended the connection - this is normal behavior\n");
                DBG("BTX: Device may have finished what it needed to do\n");

                // Special advice for Xbox controllers that disconnect with reason 0x13
                if (slot >= 0) {
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
            } else if (reason == 0x22) {
                DBG("BTX: LMP Response Timeout - device may not support our link mode\n");
                DBG("BTX: Try different link policy settings or SSP mode\n");
            } else if (reason == 0x0F) {
                DBG("BTX: Security rejection - device requires different security level\n");
                DBG("BTX: Try enabling SSP or check authentication requirements\n");
            }

            // Clean up connection tracking
            if (slot >= 0) {
                DBG("BTX: Cleaning up connection tracking for slot %d\n", slot);
                if (btx_connections[slot].hid_cid != 0) {
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
        } else {
            DBG("BTX: Disconnection complete event failed, status: 0x%02x\n", status);
        }
        break;

    case HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT:
        status = hci_event_authentication_complete_get_status(packet);
        uint16_t handle = hci_event_authentication_complete_get_connection_handle(packet);

        if (status == 0)
        {
            DBG("BTX: *** AUTHENTICATION SUCCESSFUL *** for handle: 0x%04x\n", handle);

            // Find the connection and mark it as authenticated
            int slot = find_connection_by_handle(handle);
            if (slot >= 0) {
                btx_connections[slot].authenticated = true;
                DBG("BTX: Marked connection at slot %d as authenticated\n", slot);

                // Check if this might be an Xbox controller by checking the address pattern
                // Xbox controllers often start with specific OUI patterns
                bool might_be_xbox = (btx_connections[slot].remote_addr[0] == 0x9C) ||
                                   (btx_connections[slot].remote_addr[0] == 0x58) ||
                                   (btx_connections[slot].remote_addr[0] == 0x88);

                if (might_be_xbox) {
                    DBG("BTX: Detected potential Xbox controller - using passive HID approach\n");
                    DBG("BTX: Waiting for Xbox controller to initiate HID connection to us...\n");
                    DBG("BTX: Xbox controllers prefer to establish HID connections themselves\n");
                    // Don't attempt outgoing HID connection - let Xbox controller connect to us
                } else {
                    // For other controllers, attempt normal HID connection
                    DBG("BTX: Authentication complete - now attempting HID connection\n");
                    attempt_hid_connection(slot);
                }
            } else {
                DBG("BTX: Could not find connection slot for handle 0x%04x\n", handle);
            }
        }
        else
        {
            const char *auth_error = "Unknown authentication error";
            switch (status)
            {
            case 0x04:
                auth_error = "Page Timeout";
                break;
            case 0x05:
                auth_error = "Authentication Failure";
                break;
            case 0x06:
                auth_error = "PIN or Key Missing";
                break;
            case 0x07:
                auth_error = "Memory Capacity Exceeded";
                break;
            case 0x08:
                auth_error = "Connection Timeout";
                break;
            case 0x0E:
                auth_error = "Connection Rejected - Limited Resources";
                break;
            case 0x0F:
                auth_error = "Connection Rejected - Security Reasons";
                break;
            case 0x15:
                auth_error = "Connection Terminated - Unacceptable Connection Interval";
                break;
            case 0x16:
                auth_error = "Connection Terminated by Local Host";
                break;
            case 0x17:
                auth_error = "Connection Terminated - MIC Failure";
                break;
            case 0x1F:
                auth_error = "Unspecified Error";
                break;
            case 0x25:
                auth_error = "Encryption Mode Not Acceptable";
                break;
            case 0x2A:
                auth_error = "Parameter Out of Mandatory Range";
                break;
            case 0x2F:
                auth_error = "Insufficient Security";
                break;
            }
            DBG("BTX: Authentication failed for handle: 0x%04x, status: 0x%02x (%s)\n", handle, status, auth_error);

            // Find if this is an Xbox controller for specific advice
            int slot = find_connection_by_handle(handle);
            bool is_xbox = false;
            if (slot >= 0) {
                // Check if we stored Xbox info (we could add this to connection tracking)
                is_xbox = true; // For now, assume modern controller
            }

            if (status == 0x05)
            {
                DBG("BTX: Authentication Failure - This often means PIN/passkey mismatch\n");
                if (is_xbox) {
                    DBG("BTX: Xbox controllers require SSP (Secure Simple Pairing)\n");
                    DBG("BTX: Make sure SSP is enabled and IO capabilities are set correctly\n");
                }
                DBG("BTX: Automatic PIN retry will happen if device requests pairing again\n");
                DBG("BTX: If PIN retry fails repeatedly, consider trying SSP mode\n");
            }
            else if (status == 0x06)
            {
                DBG("BTX: PIN or Key Missing - Device may need to be unpaired first\n");
                DBG("BTX: For some gamepads, try holding the pairing button longer\n");
                DBG("BTX: Automatic PIN retry will happen if device requests pairing again\n");
                DBG("BTX: This is often caused by stale pairing info on the gamepad\n");
            }
            else if (status == 0x2A)
            {
                DBG("BTX: Parameter Out of Mandatory Range - SSP configuration issue\n");
                if (is_xbox) {
                    DBG("BTX: This is common with Xbox controllers - they are very strict about SSP parameters\n");
                    DBG("BTX: Ensure IO capability is set to NoInputNoOutput and authentication requirements are correct\n");
                    DBG("BTX: Try factory resetting the Xbox controller: Hold Xbox + pairing buttons for 6+ seconds\n");
                } else {
                    DBG("BTX: This device may have incompatible SSP parameters\n");
                    DBG("BTX: Possible causes:\n");
                    DBG("BTX:   1. IO capability mismatch between host and device\n");
                    DBG("BTX:   2. Authentication requirement mismatch\n");
                    DBG("BTX:   3. Device expecting OOB data that we don't provide\n");
                    DBG("BTX:   4. Device may work better with legacy PIN pairing\n");
                    DBG("BTX: Consider trying btx_toggle_ssp() to disable SSP for this device\n");
                }
                DBG("BTX: Check SSP IO capability and authentication requirement settings\n");
            }
            else if (status == 0x2F)
            {
                DBG("BTX: Insufficient Security - Device requires stronger security\n");
                DBG("BTX: Try enabling SSP mode for this device\n");
            }
            DBG("BTX: This may prevent HID connection establishment\n");
            DBG("BTX: Consider trying SSP mode or checking if device needs factory reset\n");

            // Suggest trying SSP mode if we keep getting authentication failures
            DBG("BTX: TIP: If authentication keeps failing, try the btx_toggle_ssp() function\n");
            DBG("BTX: Some modern gamepads require SSP instead of legacy PIN pairing\n");
        }
        break;

    case HCI_EVENT_ENCRYPTION_CHANGE:
        status = packet[2];
        handle = little_endian_read_16(packet, 3);
        uint8_t encryption_enabled = packet[5];
        if (status == 0)
        {
            DBG("BTX: Encryption %s for handle: 0x%04x\n",
                encryption_enabled ? "enabled" : "disabled", handle);

            // After encryption is established, try HID connection if not already attempted
            if (encryption_enabled) {
                int slot = find_connection_by_handle(handle);
                if (slot >= 0 && !btx_connections[slot].hid_attempted) {
                    // Check if this is an Xbox controller - they need passive HID approach
                    bool might_be_xbox = (btx_connections[slot].remote_addr[0] == 0x9C) ||
                                       (btx_connections[slot].remote_addr[0] == 0x58) ||
                                       (btx_connections[slot].remote_addr[0] == 0x88);

                    if (might_be_xbox) {
                        DBG("BTX: Encryption established for Xbox controller - attempting HID connection\n");
                        DBG("BTX: Xbox controllers need HID connection initiated after encryption\n");
                        btx_connections[slot].authenticated = true;
                        // attempt_hid_connection(slot);
                    } else {
                        DBG("BTX: Encryption established - attempting HID connection as fallback\n");
                        btx_connections[slot].authenticated = true;
                        // attempt_hid_connection(slot);
                    }
                } else if (slot >= 0) {
                    DBG("BTX: Encryption established for slot %d (HID already attempted)\n", slot);
                } else {
                    DBG("BTX: Encryption established but no connection tracking found for handle 0x%04x\n", handle);
                }
            }
        }
        else
        {
            DBG("BTX: Encryption change failed for handle: 0x%04x, status: 0x%02x\n", handle, status);
        }
        break;

    case HCI_EVENT_LINK_KEY_REQUEST:
        hci_event_link_key_request_get_bd_addr(packet, event_addr);

        // Check if this is an Xbox controller for special handling
        bool is_xbox = (event_addr[0] == 0x9C) || (event_addr[0] == 0x58) || (event_addr[0] == 0x88);

        if (is_xbox) {
            DBG("BTX: Link key request from XBOX CONTROLLER %s - forcing fresh authentication\n", bd_addr_to_str(event_addr));
            DBG("BTX: This should trigger IO Capability Request and SSP authentication\n");
        } else {
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
        handle = little_endian_read_16(packet, 3);
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
        DBG("BTX: HID META EVENT - Subevent: 0x%02x\n", subevent);

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
                const char *error_desc = "Unknown error";
                switch (status)
                {
                case 0x04:
                    error_desc = "Page Timeout";
                    break;
                case 0x05:
                    error_desc = "Authentication Failure";
                    break;
                case 0x08:
                    error_desc = "Connection Timeout";
                    break;
                case 0x0E:
                    error_desc = "Connection Rejected - Limited Resources";
                    break;
                case 0x0F:
                    error_desc = "Connection Rejected - Security Reasons";
                    break;
                case 0x10:
                    error_desc = "Connection Rejected - Unacceptable BD_ADDR";
                    break;
                case 0x11:
                    error_desc = "Connection Accept Timeout Exceeded";
                    break;
                case 0x16:
                    error_desc = "Connection Terminated by Local Host";
                    break;
                case 0x22:
                    error_desc = "LMP Response Timeout";
                    break;
                case 0x28:
                    error_desc = "Instant Passed";
                    break;
                case 0x3D:
                    error_desc = "Connection Rejected - No Suitable Channel Found";
                    break;
                case 0x66:
                    error_desc = "Connection Failed to be Established";
                    break;
                }
                DBG("BTX: HID connection failed, status: 0x%02x (%s)\n", status, error_desc);

                // Find and clean up the connection attempt
                uint16_t hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
                hid_subevent_connection_opened_get_bd_addr(packet, event_addr);
                DBG("BTX: Failed HID connection was for device %s, CID: 0x%04x\n", bd_addr_to_str(event_addr), hid_cid);

                // Provide specific troubleshooting advice based on error
                if (status == 0x66) {
                    DBG("BTX: Connection Failed to be Established - Common causes:\n");
                    DBG("BTX:   1. Device not in proper pairing mode\n");
                    DBG("BTX:   2. Authentication failed earlier (check for PIN errors)\n");
                    DBG("BTX:   3. Device doesn't support HID protocol\n");
                    DBG("BTX:   4. Timing issues - try pairing again\n");
                } else if (status == 0x05) {
                    DBG("BTX: Authentication Failure - Device rejected our authentication\n");
                    DBG("BTX: Try clearing device's pairing memory and retry\n");
                } else if (status == 0x0F) {
                    DBG("BTX: Security rejection - Device may require different security mode\n");
                }

                // This device is not compatible, no need to retry
                DBG("BTX: Device may not support HID or has incompatible security requirements\n");
                break;
            }

            uint16_t hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            hid_subevent_connection_opened_get_bd_addr(packet, event_addr);

            DBG("BTX: HID_SUBEVENT_CONNECTION_OPENED - CID: 0x%04x, Address: %s\n", hid_cid, bd_addr_to_str(event_addr));

            // Find the existing connection tracking for this address
            int slot = find_connection_by_addr(event_addr);
            if (slot >= 0) {
                // Update existing connection with HID CID
                btx_connections[slot].hid_cid = hid_cid;
                btx_connections[slot].hid_attempt_time = 0; // Clear timeout since connection succeeded
                btx_connections[slot].descriptor_requested = true; // We'll wait for descriptor
                btx_connections[slot].mounted_without_descriptor = false;
                DBG("BTX: HID connection established with device at existing slot %d, CID: 0x%04x\n", slot, hid_cid);

                // Try mounting immediately with synthesized USB IDs
                uint16_t fake_vid, fake_pid;
                btx_synthesize_usb_ids(event_addr, &fake_vid, &fake_pid);
                DBG("BTX: Trying immediate mount with synthesized VID=0x%04X, PID=0x%04X\n", fake_vid, fake_pid);

                bool mounted = pad_mount(btx_slot_to_pad_idx(slot), NULL, 0, 0, fake_vid, fake_pid);
                if (mounted) {
                    DBG("BTX: *** GAMEPAD CONFIRMED! *** Successfully mounted with synthesized IDs at existing slot %d\n", slot);
                    btx_connections[slot].mounted_without_descriptor = true;
                    btx_connections[slot].descriptor_requested = false; // Don't need descriptor anymore
                } else {
                    // For devices that may never send a descriptor, we'll wait for HID reports
                    // and try to identify the gamepad from the report data itself
                    DBG("BTX: Initial mount failed - waiting for descriptor or HID reports...\n");
                    DBG("BTX: Some gamepads work without sending descriptors - we'll identify them from report data\n");
                }
            } else {
                // Fallback: create new tracking entry (shouldn't normally happen)
                for (int i = 0; i < PAD_MAX_PLAYERS; i++)
                {
                    if (!btx_connections[i].active)
                    {
                        btx_connections[i].active = true;
                        btx_connections[i].hid_cid = hid_cid;
                        memcpy(btx_connections[i].remote_addr, event_addr, BD_ADDR_LEN);
                        btx_connections[i].authenticated = true; // Assume authenticated if HID worked
                        btx_connections[i].hid_attempted = true;
                        btx_connections[i].acl_handle = 0; // Unknown at this point
                        btx_connections[i].hid_attempt_time = 0; // Connection successful
                        btx_connections[i].descriptor_requested = true; // We'll wait for descriptor
                        btx_connections[i].mounted_without_descriptor = false;

                        DBG("BTX: HID connection established with device at new slot %d, CID: 0x%04x\n", i, hid_cid);

                        // Synthesize fake USB VID/PID from Bluetooth information for pad_mount
                        uint16_t fake_vid, fake_pid;
                        btx_synthesize_usb_ids(event_addr, &fake_vid, &fake_pid);
                        DBG("BTX: Synthesized USB IDs: VID=0x%04X, PID=0x%04X\n", fake_vid, fake_pid);

                        // Try mounting without descriptor using synthesized IDs
                        DBG("BTX: Attempting gamepad mount without descriptor for new slot...\n");
                        bool mounted = pad_mount(btx_slot_to_pad_idx(i), NULL, 0, 0, fake_vid, fake_pid);
                        if (mounted) {
                            DBG("BTX: *** GAMEPAD CONFIRMED! *** Successfully mounted without descriptor at slot %d\n", i);
                            btx_connections[i].mounted_without_descriptor = true;
                            btx_connections[i].descriptor_requested = false; // Don't need descriptor anymore
                        } else {
                            DBG("BTX: Fallback mount failed - waiting for HID descriptor to determine if this is a gamepad...\n");
                        }
                        break;
                    }
                }
            }

            DBG("BTX: Connection opened successfully! Remaining discoverable for additional devices.\n");
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
                    // Check if we already mounted this device without a descriptor
                    if (btx_connections[i].mounted_without_descriptor) {
                        DBG("BTX: Device at slot %d already mounted without descriptor - ignoring descriptor event\n", i);
                        if (status == ERROR_CODE_SUCCESS) {
                            DBG("BTX: Descriptor is available but not needed for already-mounted gamepad\n");
                        } else {
                            DBG("BTX: Descriptor failed but gamepad is already working without it\n");
                        }
                        break;
                    }

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
                        uint16_t fake_vid, fake_pid;
                        btx_synthesize_usb_ids(btx_connections[i].remote_addr, &fake_vid, &fake_pid);
                        DBG("BTX: Device never sent HID descriptor - attempting fallback mount with synthesized IDs\n");
                        DBG("BTX: Synthesized VID=0x%04X, PID=0x%04X from Bluetooth info\n", fake_vid, fake_pid);

                        bool mounted = pad_mount(btx_slot_to_pad_idx(i), NULL, 0, 0, fake_vid, fake_pid);
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

        case 0x0e: // HID subevent that appears with Xbox controllers - likely CONNECTION_FAILED
        {
            uint16_t hid_cid = 0;

            // Try to extract CID from packet (usually at offset 3-4)
            if (size >= 5) {
                hid_cid = little_endian_read_16(packet, 3);
            }

            DBG("BTX: HID subevent 0x0e (CONNECTION_FAILED?) - CID: 0x%04x, size: %d\n", hid_cid, size);

            // Print the full packet for debugging
            if (size <= 16) {
                DBG("BTX: Full packet: ");
                for (int j = 0; j < size; j++) {
                    DBG("%02x ", packet[j]);
                }
                DBG("\n");
            }

            // This appears to be a connection failure specifically for Xbox controllers
            // Let's find the connection and try a different approach
            for (int i = 0; i < PAD_MAX_PLAYERS; i++) {
                if (btx_connections[i].active && btx_connections[i].hid_cid == hid_cid) {
                    DBG("BTX: HID connection failed for Xbox controller at slot %d (%s)\n",
                        i, bd_addr_to_str(btx_connections[i].remote_addr));

                    // For Xbox controllers, this might mean they need incoming HID connections instead
                    DBG("BTX: Xbox controllers sometimes initiate HID connections themselves\n");
                    DBG("BTX: Waiting for controller to initiate HID connection to us...\n");

                    // Reset HID attempt so it can try again if needed
                    btx_connections[i].hid_attempted = false;
                    btx_connections[i].hid_cid = 0;
                    btx_connections[i].hid_attempt_time = 0;

                    // Don't clean up the connection - let the controller try to connect to us
                    break;
                }
            }
        }
        break;

        default:
            DBG("BTX: Unknown HID subevent: 0x%02x (size: %d)\n", subevent, size);
            if (size <= 32) {
                DBG("BTX: HID event data: ");
                for (int i = 0; i < size; i++) {
                    DBG("%02x ", packet[i]);
                }
                DBG("\n");
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
        if (opcode == 0x0C1A)
        { // HCI_Write_Scan_Enable
            if (status == 0)
            {
                DBG("BTX: Scan enable command completed successfully\n");
            }
            else
            {
                DBG("BTX: Scan enable command failed with status: 0x%02x\n", status);
            }
        }
        // Check for inquiry command completion
        else if (opcode == 0x0401)
        { // HCI_Inquiry
            if (status == 0)
            {
                DBG("BTX: Inquiry command started successfully\n");
            }
            else
            {
                DBG("BTX: Inquiry command failed with status: 0x%02x\n", status);
            }
        }
        // Check for inquiry cancel completion
        else if (opcode == 0x0402)
        { // HCI_Inquiry_Cancel
            DBG("BTX: Inquiry cancel command completed, status: 0x%02x\n", status);
        }
        break;
    }

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

    case 0x32: // HCI_EVENT_IO_CAPABILITY_RESPONSE
    {
        hci_event_io_capability_request_get_bd_addr(packet, event_addr);
        uint8_t io_capability = packet[9];
        uint8_t oob_data_present = packet[10];
        uint8_t auth_requirement = packet[11];

        const char *io_cap_str = "Unknown";
        switch (io_capability)
        {
        case 0x00:
            io_cap_str = "DisplayOnly";
            break;
        case 0x01:
            io_cap_str = "DisplayYesNo";
            break;
        case 0x02:
            io_cap_str = "KeyboardOnly";
            break;
        case 0x03:
            io_cap_str = "NoInputNoOutput";
            break;
        }

        DBG("BTX: IO Capability Response from %s: %s, OOB: %s, Auth: 0x%02x\n",
            bd_addr_to_str(event_addr), io_cap_str,
            oob_data_present ? "Present" : "Not Present", auth_requirement);

        // Store the device's actual requirements for future reference
        int conn_slot = find_connection_by_addr(event_addr);
        if (conn_slot >= 0) {
            btx_connections[conn_slot].capability_known = true;
            btx_connections[conn_slot].device_io_capability = io_capability;
            btx_connections[conn_slot].device_oob_data_present = oob_data_present;
            btx_connections[conn_slot].device_auth_requirement = auth_requirement;
            DBG("BTX: Stored device capabilities for slot %d: IO=0x%02x, OOB=%d, Auth=0x%02x\n",
                conn_slot, io_capability, oob_data_present, auth_requirement);
        } else {
            DBG("BTX: Could not find connection slot to store device capabilities\n");
        }

        // Analyze potential compatibility issues based on actual device capabilities
        if (oob_data_present) {
            DBG("BTX: MISMATCH: Device reports OOB data present, but we sent OOB=No\n");
            DBG("BTX: This mismatch often causes authentication failures\n");
            DBG("BTX: Device expects Out-of-Band authentication data that we don't provide\n");
            DBG("BTX: Troubleshooting options:\n");
            DBG("BTX:   1. Device may need factory reset to clear OOB requirement\n");
            DBG("BTX:   2. Try pairing with another device first to clear OOB flag\n");
            DBG("BTX:   3. Some devices need specific button sequences to disable OOB mode\n");
        }

        // Check for SSP parameter mismatches
        if (io_capability != 0x00) {
            DBG("BTX: NOTE: Device capability (%s) differs from our sent capability (DisplayOnly)\n", io_cap_str);
            DBG("BTX: This may require different SSP authentication flow\n");
        }

        if (auth_requirement > 0x00) {
            DBG("BTX: Device requires authentication level 0x%02x, we sent 0x00\n", auth_requirement);
            DBG("BTX: Device has stricter authentication requirements than we configured\n");
        }

        // Provide general troubleshooting advice based on actual device response
        if (io_capability == 0x00 && oob_data_present) {
            DBG("BTX: *** CRITICAL MISMATCH: DisplayOnly + OOB Present ***\n");
            DBG("BTX: This combination often fails with Parameter Out of Range errors\n");
            DBG("BTX: Device expects OOB data for DisplayOnly authentication\n");
            DBG("BTX: Next IO Capability Request should match device requirements better\n");
        }

        DBG("BTX: Device will proceed with SSP using its reported capabilities\n");
        break;
        if (io_capability == 0x00)
        { // DisplayOnly
            DBG("BTX: Device has DisplayOnly capability - might need PIN or numeric confirmation\n");
        }
        break;
    }

    case 0x36: // HCI_EVENT_SIMPLE_PAIRING_COMPLETE
    {
        status = packet[2];
        hci_event_pin_code_request_get_bd_addr(packet, event_addr);
        if (status == 0)
        {
            DBG("BTX: *** SIMPLE PAIRING SUCCESSFUL *** with %s\n", bd_addr_to_str(event_addr));
        }
        else
        {
            const char *pairing_error = "Unknown pairing error";
            switch (status)
            {
            case 0x05:
                pairing_error = "Authentication Failure";
                break;
            case 0x06:
                pairing_error = "PIN or Key Missing";
                break;
            case 0x0F:
                pairing_error = "Connection Rejected - Security Reasons";
                break;
            case 0x2F:
                pairing_error = "Insufficient Security";
                break;
            case 0x37:
                pairing_error = "Simple Pairing Not Supported";
                break;
            }
            DBG("BTX: Simple Pairing failed with %s, status: 0x%02x (%s)\n",
                bd_addr_to_str(event_addr), status, pairing_error);
        }
        break;
    }

    case 0xd8: // HCI_EVENT_ENCRYPTION_KEY_REFRESH_COMPLETE
    {
        status = packet[2];
        handle = little_endian_read_16(packet, 3);
        if (status == 0)
        {
            DBG("BTX: Encryption key refresh successful for handle: 0x%04x\n", handle);
        }
        else
        {
            DBG("BTX: Encryption key refresh failed for handle: 0x%04x, status: 0x%02x\n", handle, status);
        }
        break;
    }

    case 0xdd: // Vendor specific event
    {
        DBG("BTX: Vendor specific event, length: %d\n", size);
        break;
    }

    case 0xff: // Vendor specific event (CYW43 specific)
    {
        // These appear to be CYW43 chip status/heartbeat events - very common and not critical
        // Pattern observed: ff 02 52 01 / ff 02 52 00 alternating
        static uint32_t vendor_event_count = 0;
        static uint32_t last_log_time = 0;

        vendor_event_count++;
        uint32_t current_time = to_ms_since_boot(get_absolute_time());

        // Only log every 1000 events or every 5 seconds to avoid spam
        if (vendor_event_count % 1000 == 1 || (current_time - last_log_time) > 5000) {
            DBG("BTX: CYW43 vendor event 0xff (count: %lu, last 5s), length: %d\n", vendor_event_count, size);
            if (size <= 8) {
                DBG("BTX: Data: ");
                for (int i = 0; i < size; i++) {
                    DBG("%02x ", packet[i]);
                }
                DBG("\n");
            }
            last_log_time = current_time;
        }
        break;
    }

    case 0xe1: // HCI_EVENT_SNIFF_SUBRATING
    {
        status = packet[2];
        handle = little_endian_read_16(packet, 3);
        if (status == 0)
        {
            DBG("BTX: Sniff subrating configured for handle: 0x%04x\n", handle);
        }
        else
        {
            DBG("BTX: Sniff subrating failed for handle: 0x%04x, status: 0x%02x\n", handle, status);
        }
        break;
    }

    case 0x73: // Unknown event that appears during disconnection
    {
        DBG("BTX: Event 0x73 (possibly related to disconnection), handle: 0x%04x\n",
            little_endian_read_16(packet, 2));
        // This event often appears right before disconnection
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
            event_type != 0xff) // Skip vendor-specific events (handled above)
        {
            DBG("BTX: Unhandled HCI event 0x%02x (size: %d)\n", event_type, size);
            // Print first few bytes for debugging only for potentially important events
            if (size <= 16)
            {
                DBG("BTX: Event data: ");
                for (int i = 0; i < size; i++)
                {
                    DBG("%02x ", packet[i]);
                }
                DBG("\n");
            }
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

    // Clear PIN retry tracking array
    memset(pin_retries, 0, sizeof(pin_retries));

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
    DBG("BTX: SSP authentication set to no MITM protection (global default for Xbox compatibility)\n");    // Set bondable mode for both SSP and legacy pairing
    gap_set_bondable_mode(1);
    DBG("BTX: Bondable mode enabled\n");

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
    for (int i = 0; i < PAD_MAX_PLAYERS; i++) {
        if (btx_connections[i].active) {
            // Initialize timing for new connections
            if (connection_start_time[i] == 0) {
                connection_start_time[i] = now;
                auth_attempted[i] = false;
            }

            // If we've had an ACL connection for more than 3 seconds without authentication
            // and we haven't already tried manual authentication
            if (!btx_connections[i].authenticated &&
                !auth_attempted[i] &&
                (now - connection_start_time[i]) > 3000) {

                DBG("BTX: Connection at slot %d has been waiting for authentication for >3s\n", i);
                DBG("BTX: Device: %s, Handle: 0x%04x\n",
                    bd_addr_to_str(btx_connections[i].remote_addr),
                    btx_connections[i].acl_handle);
                DBG("BTX: Attempting to trigger authentication manually (one time only)...\n");

                // Try to trigger authentication manually - but only once
                hci_send_cmd(&hci_authentication_requested, btx_connections[i].acl_handle);
                auth_attempted[i] = true;
            }
        } else if (connection_start_time[i] != 0) {
            // Reset tracking when connection becomes inactive
            connection_start_time[i] = 0;
            auth_attempted[i] = false;
        }
    }    // Check for HID connection timeouts
    for (int i = 0; i < PAD_MAX_PLAYERS; i++) {
        if (btx_connections[i].active &&
            btx_connections[i].authenticated &&
            btx_connections[i].hid_attempted &&
            btx_connections[i].hid_cid != 0 &&
            btx_connections[i].hid_attempt_time > 0) {

            // Check if HID connection has been pending for more than 10 seconds
            if (now - btx_connections[i].hid_attempt_time > 10000) {
                DBG("BTX: HID connection timeout for slot %d (CID: 0x%04x) - no response after 10 seconds\n",
                    i, btx_connections[i].hid_cid);
                DBG("BTX: This suggests the device may not support HID protocol\n");
                DBG("BTX: Cleaning up stalled connection attempt\n");

                // Clean up the stalled attempt
                if (btx_connections[i].hid_cid != 0) {
                    hid_host_disconnect(btx_connections[i].hid_cid);
                }
                btx_connections[i].hid_attempted = false;
                btx_connections[i].hid_cid = 0;
                btx_connections[i].hid_attempt_time = 0;

                // Could try a different approach here, but for now just give up on this device
                DBG("BTX: Device at slot %d does not appear to support HID - giving up\n", i);
            }
        }
    }

    // Periodically ensure we stay discoverable and connectable
    // holy fuck don't do this
    // static uint32_t last_status_check = 0;
    // uint32_t now = to_ms_since_boot(get_absolute_time());

    // if (now - last_status_check > 30000)
    // { // Every 30 seconds
    //     last_status_check = now;

    //     // Re-enable discoverable/connectable mode to ensure it stays active
    //     gap_discoverable_control(1);
    //     gap_connectable_control(1);
    //     DBG("BTX: Status check - device should be visible as 'RP6502-Console'\n");
    //     DBG("BTX: CoD: 0x002540 (Computer with HID), Legacy pairing enabled\n");
    //     DBG("BTX: Advertising HID service - gamepads should see us as HID-capable device\n");
    //     DBG("BTX: Also performing active gamepad discovery when pairing mode enabled\n");
    //     DBG("BTX: Try scanning for Bluetooth devices on your phone to verify visibility\n");

    //     // Also make sure scan modes are still enabled
    //     hci_send_cmd(&hci_write_scan_enable, 0x03); // Both inquiry and page scan
    // }
}

bool btx_start_pairing(void)
{
    if (!btx_initialized)
    {
        DBG("BTX: Cannot start pairing - not initialized\n");
        return false;
    }

    // Clear any existing link keys to prevent "PIN or Key Missing" errors
    // This is especially important for Xbox One gamepads and other devices
    // that may have stale bonding information from previous pairing attempts
    gap_delete_all_link_keys();
    DBG("BTX: Cleared all existing link keys to prevent authentication errors\n");

    // Clear PIN retry tracking for fresh start
    memset(pin_retries, 0, sizeof(pin_retries));
    DBG("BTX: Reset PIN retry tracking for fresh pairing session\n");

    btx_pairing_mode = true;

    // Make sure we're discoverable and connectable for incoming connections
    gap_discoverable_control(1);
    gap_connectable_control(1);

    DBG("BTX: *** STARTING ACTIVE GAMEPAD SEARCH ***\n");
    DBG("BTX: Put your gamepad in pairing mode now\n");
    DBG("BTX: Device is also discoverable as 'RP6502-Console' for reverse connections\n");
    DBG("BTX: NOTE: Only devices advertising HID service or Peripheral class will be considered\n");
    DBG("BTX: This helps avoid connecting to computers and phones during inquiry\n");
    DBG("BTX: \n");
    DBG("BTX: DEBUG MODE: Currently accepting ALL incoming connections\n");
    DBG("BTX: This should allow 8BitDo and other problematic gamepads to connect\n");
    DBG("BTX: For 8BitDo gamepads: Make sure to put them in proper pairing mode\n");
    DBG("BTX: Many gamepads connect TO us rather than being discovered in inquiry\n");
    DBG("BTX: If no devices found in inquiry, try putting gamepad in pairing mode again\n");
    DBG("BTX: \n");
    DBG("BTX: TROUBLESHOOTING TIPS:\n");
    DBG("BTX:   1. If PIN authentication fails, try holding gamepad pairing button longer\n");
    DBG("BTX:   2. Some gamepads need to be factory reset before pairing\n");
    DBG("BTX:   3. Try different button combinations for pairing mode\n");
    DBG("BTX:   4. If connection fails (0x66), the device may not support HID properly\n");
    DBG("BTX:   5. If PIN keeps failing, try SSP mode with btx_toggle_ssp() function\n");
    DBG("BTX:   6. Modern gamepads (PS4/PS5/Xbox One) may need SSP instead of PIN\n");

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

void btx_toggle_ssp(void)
{
    if (!btx_initialized)
    {
        DBG("BTX: Cannot toggle SSP - not initialized\n");
        return;
    }

    static bool ssp_enabled = true; // Start with SSP enabled by default
    ssp_enabled = !ssp_enabled;

    gap_ssp_set_enable(ssp_enabled ? 1 : 0);

    if (ssp_enabled) {
        DBG("BTX: SSP (Secure Simple Pairing) ENABLED\n");
        DBG("BTX: This may work better with modern gamepads (PS4/PS5/Xbox One)\n");
        DBG("BTX: Devices will use numeric confirmation instead of PIN\n");
    } else {
        DBG("BTX: SSP (Secure Simple Pairing) DISABLED\n");
        DBG("BTX: Using legacy PIN-based pairing (better for older gamepads)\n");
        DBG("BTX: Devices will need to use PIN '0000'\n");
    }

    // Clear link keys when changing security mode
    gap_delete_all_link_keys();
    DBG("BTX: Cleared link keys due to security mode change\n");
}

#endif /* RP6502_RIA_W */
