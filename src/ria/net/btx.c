/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W
#include "net/wfi.h"
void btx_task(void) {}
void btx_reset(void) {}
void btx_start_pairing(void) {}
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
    absolute_time_t addr_valid_until;
    bd_addr_t remote_addr;
    uint16_t hid_cid; // HID connection ID, BTStack leaves 0 for unused
} btx_connection_t;

static btx_connection_t btx_connections[MAX_NR_HCI_CONNECTIONS];
static bool btx_initialized;

// Although everything looks like it support mutiple gamepads,
// once HID is opened the global SDP connection is locked to a
// single gamepad. If a second gamepad tries to hid_host_connect
// then the btstack stops requesting HID descriptors forever.
static bool btx_subevent_opened;

// BTStack state - Classic HID Host
static btstack_packet_callback_registration_t hci_event_callback_registration;

// Storage for HID descriptors - Classic only
static uint8_t hid_descriptor_storage[512]; // HID descriptor storage

static int find_connection_by_hid_cid(uint16_t hid_cid)
{
    for (int i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
        if (btx_connections[i].hid_cid == hid_cid)
            return i;
    return -1;
}

static int find_connection_by_addr(bd_addr_t addr)
{
    for (int i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
        if (memcmp(btx_connections[i].remote_addr, addr, BD_ADDR_LEN) == 0 &&
            absolute_time_diff_us(btx_connections[i].addr_valid_until,
                                  get_absolute_time()) < 0)
            return i;
    return -1;
}

#define BTX_CONNECTION_TIMEOUT_SECS 10 // TODO 10? 60?

static int create_connection_entry(bd_addr_t addr)
{
    for (int i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
    {
        if (btx_connections[i].hid_cid == 0 &&
            absolute_time_diff_us(btx_connections[i].addr_valid_until,
                                  get_absolute_time()) > 0)
        {

            btx_connections[i].addr_valid_until = make_timeout_time_ms(BTX_CONNECTION_TIMEOUT_SECS * 1000);
            memcpy(btx_connections[i].remote_addr, addr, BD_ADDR_LEN);
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
        if (state == HCI_STATE_WORKING)
        {
            DBG("BTX: Bluetooth Classic HID Host ready and working!\n");
            gap_discoverable_control(1);
            gap_connectable_control(1);
        }
        break;

    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(packet, event_addr);
        const char *pin = "0000"; // Always 0000
        DBG("BTX: HCI_EVENT_PIN_CODE_REQUEST from %s, using PIN '%s'\n", bd_addr_to_str(event_addr), pin);
        gap_pin_code_response(event_addr, pin);
        break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
        uint32_t numeric_value = hci_event_user_confirmation_request_get_numeric_value(packet);
        DBG("BTX: HCI_EVENT_USER_CONFIRMATION_REQUEST from %s: %lu\n", bd_addr_to_str(event_addr), (unsigned long)numeric_value);
        gap_ssp_confirmation_response(event_addr);
        break;

    case HCI_EVENT_USER_PASSKEY_REQUEST:
        hci_event_user_passkey_request_get_bd_addr(packet, event_addr);
        DBG("BTX: HCI_EVENT_USER_PASSKEY_REQUEST from %s - using 0\n", bd_addr_to_str(event_addr));
        hci_send_cmd(&hci_user_passkey_request_reply, event_addr, 0);
        break;

    case HCI_EVENT_IO_CAPABILITY_REQUEST:
        hci_event_io_capability_request_get_bd_addr(packet, event_addr);
        DBG("BTX: HCI_EVENT_IO_CAPABILITY_REQUEST\n");
        uint8_t io_capability = SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT;
        uint8_t oob_data_present = 0x00;
        uint8_t auth_requirement = SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING;
        hci_send_cmd(&hci_io_capability_request_reply, event_addr, io_capability, oob_data_present, auth_requirement);
        break;

    case HCI_EVENT_INQUIRY_COMPLETE:
        DBG("BTX: HCI_EVENT_INQUIRY_COMPLETE\n");
        break;

    case HCI_EVENT_INQUIRY_RESULT:
    case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
    case HCI_EVENT_EXTENDED_INQUIRY_RESPONSE:
    {
        hci_event_inquiry_result_get_bd_addr(packet, event_addr);
        DBG("BTX: HCI_EVENT_INQUIRY_RESULT from %s\n", bd_addr_to_str(event_addr));

        uint32_t cod = hci_event_inquiry_result_get_class_of_device(packet);
        bool has_hid_service = (cod & (1 << 13)) != 0;
        DBG("BTX: HID service bit: %s\n", has_hid_service ? "Present" : "Not present");

        // don't create_connection_entry now so we try to hid_host_connect later
        hci_send_cmd(&hci_create_connection, event_addr, 0xCC18, 0x01, 0x00, 0x00, 0x01);
        break;
    }

    case HCI_EVENT_CONNECTION_REQUEST:
        hci_event_connection_request_get_bd_addr(packet, event_addr);
        uint32_t cod = hci_event_connection_request_get_class_of_device(packet);
        DBG("BTX: HCI_EVENT_CONNECTION_REQUEST from %s, CoD: 0x%06lx\n", bd_addr_to_str(event_addr), (unsigned long)cod);

        // This doesn't work, xbox has this bit off
        bool has_hid_service = (cod & (1 << 13)) != 0;
        DBG("BTX: HID service bit: %s\n", has_hid_service ? "Present" : "Not present");

        // create_connection_entry now so we don't try to hid_host_connect later
        create_connection_entry(event_addr);
        hci_send_cmd(&hci_accept_connection_request, event_addr, HCI_ROLE_MASTER);
        break;

    case HCI_EVENT_CONNECTION_COMPLETE:
        DBG("BTX: HCI_EVENT_CONNECTION_COMPLETE\n");
        hci_event_connection_complete_get_bd_addr(packet, event_addr);
        status = hci_event_connection_complete_get_status(packet);
        if (btx_subevent_opened)
        {
            uint16_t connection_handle = hci_event_connection_complete_get_connection_handle(packet);
            DBG("BTX: Already have active connection, disconnecting (handle: 0x%04x)\n", connection_handle);
            hci_send_cmd(&hci_disconnect, connection_handle, ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION);
        }
        else if (status == 0)
        {
            // Only process ACL connections for gamepads (link_type == 0x01)
            uint8_t link_type = hci_event_connection_complete_get_link_type(packet);
            if (link_type != 0x01)
            {
                DBG("BTX: Ignoring non-ACL connection (link_type: 0x%02x)\n", link_type);
                break;
            }

            // Find the existing connection entry created during inquiry or connection request
            int slot = find_connection_by_addr(event_addr);
            if (slot < 0)
            {
                DBG("BTX: Initiating HID connection\n");
                slot = create_connection_entry(event_addr);
                if (slot < 0)
                {
                    DBG("BTX: No slot available, should not happen\n");
                    break;
                }
                uint8_t hid_status = hid_host_connect(event_addr,
                                                      HID_PROTOCOL_MODE_REPORT,
                                                      &btx_connections[slot].hid_cid);
                if (hid_status != ERROR_CODE_SUCCESS)
                {
                    DBG("BTX: Failed to initiate HID connection to %s, status: 0x%02x\n",
                        bd_addr_to_str(event_addr), hid_status);
                }
            }
            else
            {
                DBG("BTX: Waiting for HID connection\n");
            }
            // Refresh timeout
            btx_connections[slot].addr_valid_until = make_timeout_time_ms(BTX_CONNECTION_TIMEOUT_SECS * 1000);
        }
        break;

    case HCI_EVENT_AUTHENTICATION_COMPLETE:
        DBG("BTX: HCI_EVENT_AUTHENTICATION_COMPLETE\n");
        break;

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        DBG("BTX: HCI_EVENT_DISCONNECTION_COMPLETE\n");
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
            DBG("BTX: HID_SUBEVENT_INCOMING_CONNECTION from %s, CID: 0x%04x\n", bd_addr_to_str(event_addr), hid_cid);

            // Find the existing ACL connection and store the hid_cid
            int slot = find_connection_by_addr(event_addr);
            if (slot >= 0)
            {
                btx_connections[slot].hid_cid = hid_cid;
                btx_connections[slot].addr_valid_until = 0;
                DBG("BTX: Stored HID CID 0x%04x for connection slot %d\n", hid_cid, slot);
            }

            // Always accept incoming HID connections when discoverable (BTStack pattern)
            hid_host_accept_connection(hid_cid, HID_PROTOCOL_MODE_REPORT);
        }
        break;

        case HID_SUBEVENT_REPORT:
        {
            uint16_t hid_cid = hid_subevent_report_get_hid_cid(packet);
            const uint8_t *report = hid_subevent_report_get_report(packet);
            uint16_t report_len = hid_subevent_report_get_report_len(packet);

            int slot = find_connection_by_hid_cid(hid_cid);
            if (slot >= 0 && report_len)
                pad_report(btx_slot_to_pad_idx(slot), report + 1, report_len - 1);
        }
        break;

        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
        {
            uint16_t hid_cid = hid_subevent_descriptor_available_get_hid_cid(packet);
            status = hid_subevent_descriptor_available_get_status(packet);

            DBG("BTX: HID_SUBEVENT_DESCRIPTOR_AVAILABLE - CID: 0x%04x, Status: 0x%02x\n", hid_cid, status);

            int slot = find_connection_by_hid_cid(hid_cid);
            if (slot >= 0)
            {
                if (status == ERROR_CODE_SUCCESS)
                {
                    const uint8_t *descriptor = hid_descriptor_storage_get_descriptor_data(hid_cid);
                    uint16_t descriptor_len = hid_descriptor_storage_get_descriptor_len(hid_cid);
                    bool mounted = pad_mount(btx_slot_to_pad_idx(slot), descriptor, descriptor_len, 0, 0, 0);
                    if (mounted)
                    {
                        DBG("BTX: *** GAMEPAD CONFIRMED! *** Successfully mounted at slot %d\n", slot);
                        break;
                    }
                }
            }
            DBG("BTX: Failed to get HID descriptor for device at slot %d, status: 0x%02x\n", slot, status);
            hid_host_disconnect(hid_cid);
        }
        break;

        case HID_SUBEVENT_CONNECTION_OPENED:
        {
            uint8_t status = hid_subevent_connection_opened_get_status(packet);
            uint16_t hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            DBG("BTX: HID_SUBEVENT_CONNECTION_OPENED - CID: 0x%04x, status: 0x%02x\n", hid_cid, status);
            if (status == ERROR_CODE_SUCCESS)
            {
                btx_subevent_opened = true;
            }
            else
            {
                DBG("BTX: HID connection failed, status: 0x%02x\n", status);
            }
        }
        break;

        case HID_SUBEVENT_CONNECTION_CLOSED:
        {
            uint16_t hid_cid = hid_subevent_connection_closed_get_hid_cid(packet);
            DBG("BTX: HID_SUBEVENT_CONNECTION_CLOSED (0x03) - CID: 0x%04x\n", hid_cid);

            btx_subevent_opened = false;

            int slot = find_connection_by_hid_cid(hid_cid);
            if (slot >= 0)
            {
                pad_umount(btx_slot_to_pad_idx(slot));
                btx_connections[slot].hid_cid = 0;
                DBG("BTX: HID connection closed for slot %d\n", slot);
            }
        }
        break;

        default:
            // DBG("BTX: Unhandled HID subevent: 0x%02x (size: %d)\n", subevent, size);
            // if (size <= 32)
            // {
            //     DBG("BTX: HID event data: ");
            //     for (int i = 0; i < size; i++)
            //     {
            //         DBG("%02x ", packet[i]);
            //     }
            //     DBG("\n");
            // }
            break;
        }
    }
    break;

    default:
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

    // Set default link policy to allow sniff mode
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    DBG("BTX: Link policy configured for sniff mode\n");

    // Try to become master on incoming connections (from BTStack example)
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    DBG("BTX: Master/slave policy set to prefer master role\n");

    // Set Class of Device to indicate HID capability
    // 0x002540 = Computer Major Class (0x01), Desktop Minor Class (0x01), with HID service bit set (0x02)
    // This tells gamepads we're a computer that accepts HID connections
    // TODO verify
    gap_set_class_of_device(0x002540);
    gap_set_local_name("RP6502");
    DBG("BTX: Class of Device (computer with HID) and name configured\n");

    // Enable SSP by default for modern gamepads
    gap_ssp_set_enable(1); // Enable SSP for modern gamepad compatibility

    // This is the GLOBAL default that will be used when IO Capability Request handler doesn't trigger
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);

    // Set authentication requirements for SSP - use dedicated bonding for better gamepad compatibility
    // Many gamepads expect bonding to store the pairing information permanently
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_set_bondable_mode(1);
    DBG("BTX: Bondable mode enabled\n");

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
        btx_initialized = true;
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

    // Try a simple inquiry first to see if the command works at all
    DBG("BTX: Attempting inquiry with LAP 0x9E8B33, length 0x08 (10.24s), num_responses 0x00 (unlimited)\n");
    // 0x9E8B33: General/Unlimited Inquiry Access Code (GIAC)
    // TODO what is correct length? 5-15 in examples
    uint8_t result = hci_send_cmd(&hci_inquiry, 0x9E8B33, 0x08, 0x00);
    DBG("BTX: hci_send_cmd returned: %d\n", result);

    // // Enable inquiry scan mode explicitly for better gamepad compatibility
    // hci_send_cmd(&hci_write_scan_enable, 0x03); // Both inquiry and page scan
    // DBG("BTX: Enabled both inquiry and page scan modes\n");

    // // Set inquiry scan parameters for better visibility
    // // Make inquiry scan more frequent and longer window for better gamepad discovery
    // hci_send_cmd(&hci_write_inquiry_scan_activity, 0x1000, 0x0800); // interval=0x1000, window=0x0800
    // DBG("BTX: Enhanced inquiry scan parameters for better gamepad discovery\n");

    return true;
}

void btx_cyw_resetting(void)
{
    btx_initialized = false;
    for (int i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
    {
        if (btx_connections[i].hid_cid)
        {
            // Clean up gamepad registration
            pad_umount(btx_slot_to_pad_idx(i));
            btx_connections[i].hid_cid = 0;
        }
    }
    DBG("BTX: All Bluetooth gamepad connections disconnected\n");
}

void btx_reset(void)
{
    // TODO shutdown pairing session?
}

#endif /* RP6502_RIA_W */
