/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "btstack.h"
#include "net/btc.h"

#if !defined(RP6502_RIA_W) || !defined(ENABLE_CLASSIC)
void btc_task(void) {}
void btc_print_status() {}
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
#include "sys/cfg.h"
#include "usb/pad.h"
#include <stdio.h>
#include <string.h>
#include "pico/time.h"

// Although everything looks like it support mutiple gamepads,
// once HID is opened the global SDP connection is locked to a
// single gamepad. If a second gamepad tries to hid_host_connect
// then the btstack stops requesting HID descriptors forever.
static_assert(MAX_NR_HID_HOST_CONNECTIONS == 1);

// We can use the same indexing as hid and xin so long as we keep clear
static uint8_t btc_slot_to_pad_idx(int slot)
{
    return CFG_TUH_HID + PAD_MAX_PLAYERS + slot;
}

// Connection tracking for Classic HID Host
#define BTC_CONNECTION_TIMEOUT_SECS 6
#define BTC_HCI_TO_HID_TIMEOUT_SECS 10
typedef struct
{
    // Until a connection has hid_cid, it is at risk of timing out
    absolute_time_t addr_valid_until;
    bd_addr_t remote_addr;
    // HID connection ID, BTStack leaves 0 for unused
    uint16_t hid_cid;
} btc_connection_t;

static btc_connection_t btc_connections[MAX_NR_HCI_CONNECTIONS];
static bool btc_initialized;
static bool btc_pairing;
static absolute_time_t btc_next_inquiry;

// BTStack state - Classic HID Host
static btstack_packet_callback_registration_t hci_event_callback_registration;

// Storage for HID descriptors - Classic only
static uint8_t hid_descriptor_storage[512]; // HID descriptor storage

static int find_connection_by_hid_cid(uint16_t hid_cid)
{
    for (int i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
        if (btc_connections[i].hid_cid == hid_cid)
            return i;
    return -1;
}

static int find_connection_by_addr(bd_addr_t addr)
{
    for (int i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
        if (memcmp(btc_connections[i].remote_addr, addr, BD_ADDR_LEN) == 0 &&
            absolute_time_diff_us(btc_connections[i].addr_valid_until,
                                  get_absolute_time()) < 0)
            return i;
    return -1;
}

static int create_connection_entry(bd_addr_t addr)
{
    for (int i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
    {
        if (btc_connections[i].hid_cid == 0 &&
            absolute_time_diff_us(btc_connections[i].addr_valid_until,
                                  get_absolute_time()) > 0)
        {

            memcpy(btc_connections[i].remote_addr, addr, BD_ADDR_LEN);
            btc_connections[i].addr_valid_until = make_timeout_time_ms(BTC_CONNECTION_TIMEOUT_SECS * 1000);
            return i;
        }
    }
    return -1;
}

static int btc_num_connected(void)
{
    int num = 0;
    for (int i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
        if (btc_connections[i].hid_cid)
            ++num;
    return num;
}

static void btc_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
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
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING)
        {
            DBG("BTC: Bluetooth Classic HID Host ready and working!\n");
            gap_connectable_control(1);
        }
        break;

    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(packet, event_addr);
        const char *pin = "0000"; // Always 0000
        DBG("BTC: HCI_EVENT_PIN_CODE_REQUEST from %s\n", bd_addr_to_str(event_addr));
        if (btc_pairing)
            gap_pin_code_response(event_addr, pin);
        else
            gap_pin_code_negative(event_addr);
        break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
        DBG("BTC: HCI_EVENT_USER_CONFIRMATION_REQUEST from %s\n", bd_addr_to_str(event_addr));
        if (btc_pairing)
            gap_ssp_confirmation_response(event_addr);
        else
            gap_ssp_confirmation_negative(event_addr);

        break;

    case HCI_EVENT_USER_PASSKEY_REQUEST:
        hci_event_user_passkey_request_get_bd_addr(packet, event_addr);
        DBG("BTC: HCI_EVENT_USER_PASSKEY_REQUEST from %s\n", bd_addr_to_str(event_addr));
        if (btc_pairing)
            hci_send_cmd(&hci_user_passkey_request_reply, event_addr, 0);
        else
            hci_send_cmd(&hci_user_passkey_request_negative_reply, event_addr, 0);
        break;

    case HCI_EVENT_INQUIRY_COMPLETE:
        DBG("BTC: HCI_EVENT_INQUIRY_COMPLETE\n");
        break;

    case HCI_EVENT_INQUIRY_RESULT:
    case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
    {
        hci_event_inquiry_result_get_bd_addr(packet, event_addr);
        DBG("BTC: HCI_EVENT_INQUIRY_RESULT from %s\n", bd_addr_to_str(event_addr));

        uint32_t cod = hci_event_inquiry_result_get_class_of_device(packet);
        bool has_hid_service = (cod & (1 << 13)) != 0;
        DBG("BTC: HID service bit: %s\n", has_hid_service ? "Present" : "Not present");

        // don't create_connection_entry now so we try to hid_host_connect later
        hci_send_cmd(&hci_create_connection, event_addr, 0xCC18, 0x01, 0x00, 0x00, 0x01);
        break;
    }

    case HCI_EVENT_CONNECTION_REQUEST:
        hci_event_connection_request_get_bd_addr(packet, event_addr);
        uint32_t cod = hci_event_connection_request_get_class_of_device(packet);
        DBG("BTC: HCI_EVENT_CONNECTION_REQUEST from %s, CoD: 0x%06lx\n", bd_addr_to_str(event_addr), (unsigned long)cod);

        // This doesn't work, xbox has this bit off
        bool has_hid_service = (cod & (1 << 13)) != 0;
        DBG("BTC: HID service bit: %s\n", has_hid_service ? "Present" : "Not present");

        // create_connection_entry now so we don't try to hid_host_connect later
        create_connection_entry(event_addr);
        hci_send_cmd(&hci_accept_connection_request, event_addr, HCI_ROLE_MASTER);
        break;

    case HCI_EVENT_CONNECTION_COMPLETE:
        DBG("BTC: HCI_EVENT_CONNECTION_COMPLETE\n");
        hci_event_connection_complete_get_bd_addr(packet, event_addr);
        if (!hci_event_connection_complete_get_status(packet))
        {
            // Only process ACL connections for gamepads (link_type == 0x01)
            uint8_t link_type = hci_event_connection_complete_get_link_type(packet);
            if (link_type != 0x01)
            {
                DBG("BTC: Ignoring non-ACL connection (link_type: 0x%02x)\n", link_type);
                break;
            }

            // Find the existing connection entry created during inquiry or connection request
            int slot = find_connection_by_addr(event_addr);
            if (slot < 0)
            {
                DBG("BTC: Initiating HID connection\n");
                slot = create_connection_entry(event_addr);
                if (slot < 0)
                {
                    DBG("BTC: No slot available, should not happen\n");
                    break;
                }
                uint8_t hid_status = hid_host_connect(event_addr,
                                                      HID_PROTOCOL_MODE_REPORT,
                                                      &btc_connections[slot].hid_cid);
                if (hid_status != ERROR_CODE_SUCCESS)
                {
                    DBG("BTC: Failed to initiate HID connection to %s, status: 0x%02x\n",
                        bd_addr_to_str(event_addr), hid_status);
                }
            }
            else
            {
                DBG("BTC: Waiting for HID connection\n");
            }
            // Refresh timeout
            btc_connections[slot].addr_valid_until = make_timeout_time_ms(BTC_HCI_TO_HID_TIMEOUT_SECS * 1000);
        }
        break;

    case HCI_EVENT_AUTHENTICATION_COMPLETE:
        DBG("BTC: HCI_EVENT_AUTHENTICATION_COMPLETE\n");
        status = hci_event_authentication_complete_get_status(packet);
        // On success, turn off pairing mode
        if (status == 0)
            btc_pairing = false;
        break;

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        DBG("BTC: HCI_EVENT_DISCONNECTION_COMPLETE\n");
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
            DBG("BTC: HID_SUBEVENT_INCOMING_CONNECTION from %s, CID: 0x%04x\n", bd_addr_to_str(event_addr), hid_cid);

            // Find the existing ACL connection and store the hid_cid
            int slot = find_connection_by_addr(event_addr);
            if (slot >= 0)
            {
                btc_connections[slot].hid_cid = hid_cid;
                btc_connections[slot].addr_valid_until = 0;
                DBG("BTC: Stored HID CID 0x%04x for connection slot %d\n", hid_cid, slot);
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
                pad_report(btc_slot_to_pad_idx(slot), report + 1, report_len - 1);
        }
        break;

        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
        {
            uint16_t hid_cid = hid_subevent_descriptor_available_get_hid_cid(packet);
            status = hid_subevent_descriptor_available_get_status(packet);

            DBG("BTC: HID_SUBEVENT_DESCRIPTOR_AVAILABLE - CID: 0x%04x, Status: 0x%02x\n", hid_cid, status);

            int slot = find_connection_by_hid_cid(hid_cid);
            if (slot >= 0)
            {
                if (status == ERROR_CODE_SUCCESS)
                {
                    const uint8_t *descriptor = hid_descriptor_storage_get_descriptor_data(hid_cid);
                    uint16_t descriptor_len = hid_descriptor_storage_get_descriptor_len(hid_cid);
                    bool mounted = pad_mount(btc_slot_to_pad_idx(slot), descriptor, descriptor_len, 0, 0, 0);
                    if (mounted)
                    {
                        btc_pairing = false;
                        DBG("BTC: *** GAMEPAD CONFIRMED! *** Successfully mounted at slot %d\n", slot);
                        break;
                    }
                }
            }
            DBG("BTC: Failed to get HID descriptor for device at slot %d, status: 0x%02x\n", slot, status);
            hid_host_disconnect(hid_cid);
        }
        break;

        case HID_SUBEVENT_CONNECTION_OPENED:
        {
            uint8_t status = hid_subevent_connection_opened_get_status(packet);
            uint16_t hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            DBG("BTC: HID_SUBEVENT_CONNECTION_OPENED - CID: 0x%04x, status: 0x%02x\n", hid_cid, status);
            if (status != ERROR_CODE_SUCCESS)
            {
                DBG("BTC: HID connection failed, status: 0x%02x\n", status);
                int slot = find_connection_by_hid_cid(hid_cid);
                if (slot >= 0)
                {
                    btc_connections[slot].hid_cid = 0;
                    DBG("BTC: Cleaned up failed connection slot %d\n", slot);
                }
            }
        }
        break;

        case HID_SUBEVENT_CONNECTION_CLOSED:
        {
            uint16_t hid_cid = hid_subevent_connection_closed_get_hid_cid(packet);
            DBG("BTC: HID_SUBEVENT_CONNECTION_CLOSED (0x03) - CID: 0x%04x\n", hid_cid);
            int slot = find_connection_by_hid_cid(hid_cid);
            if (slot >= 0)
            {
                pad_umount(btc_slot_to_pad_idx(slot));
                btc_connections[slot].hid_cid = 0;
                DBG("BTC: HID connection closed for slot %d\n", slot);
            }
        }
        break;

        default:
            // DBG("BTC: Unhandled HID subevent: 0x%02x (size: %d)\n", subevent, size);
            // if (size <= 32)
            // {
            //     DBG("BTC: HID event data: ");
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
        // DBG("BTC: Unhandled HCI event 0x%02x (size: %d)\n", event_type, size);
        // // Print first few bytes for debugging only for potentially important events
        // if (size <= 16)
        // {
        //     DBG("BTC: Event data: ");
        //     for (int i = 0; i < size; i++)
        //     {
        //         DBG("%02x ", packet[i]);
        //     }
        //     DBG("\n");
        // }
        break;
    }
}

static void btc_init_stack(void)
{
    // Clear connection array
    memset(btc_connections, 0, sizeof(btc_connections));

    // Note: BTStack memory and run loop are automatically initialized by pico_btstack_cyw43
    // when cyw43_arch_init() is called. We don't need to do it again here.

    // Initialize L2CAP (required for HID Host) - MUST be first
    l2cap_init();

    // Initialize SDP Server (needed for service records)
    sdp_init();

    // Initialize HID Host BEFORE setting GAP parameters
    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
    hid_host_register_packet_handler(btc_packet_handler);

    // Register for HCI events BEFORE configuring GAP
    hci_event_callback_registration.callback = &btc_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // Set default link policy to allow sniff mode
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE);

    // Remaining master is the role we want
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    gap_set_allow_role_switch(false);

    //  * Computer Major Class (0x01)
    //  * Desktop Minor Class (0x01)
    //  * HID service bit 13 set
    gap_set_class_of_device(0x002140);

    // Enable SSP for modern gamepads
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_set_bondable_mode(1);

    // Start the Bluetooth stack
    hci_power_control(HCI_POWER_ON);

    DBG("BTC: Initialized\n");
}

void btc_task(void)
{
    if (!btc_initialized && cyw_ready() && cfg_get_bt())
    {
        btc_init_stack();
        btc_initialized = true;
        return;
    }

    // Handle periodic inquiry while in pairing mode
    if (btc_initialized && btc_pairing)
    {
        if (absolute_time_diff_us(btc_next_inquiry, get_absolute_time()) > 0)
        {
            // 0x9E8B33: General/Unlimited Inquiry Access Code (GIAC)
            static const int BTC_INQUIRY_LAP = 0x9E8B33;
            // 0x05: Inquiry length (6.4s)
            static const int BTC_INQUIRY_LEN = 0x05;
            hci_send_cmd(&hci_inquiry, BTC_INQUIRY_LAP, BTC_INQUIRY_LEN, 0x00);
            btc_next_inquiry = make_timeout_time_ms(10000); // 10 seconds
        }
    }
}

void btc_set_config(uint8_t bt)
{
    if (bt == 0)
        btc_shutdown();
    if (bt == 2 && !btc_num_connected())
        btc_pairing = true;
    else
        btc_pairing = false;
}

void btc_shutdown(void)
{
    if (btc_initialized)
        hci_power_control(HCI_POWER_OFF);
    btc_initialized = false;
    for (int i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
    {
        if (btc_connections[i].hid_cid)
        {
            pad_umount(btc_slot_to_pad_idx(i));
            btc_connections[i].hid_cid = 0;
        }
    }
    DBG("BTC: All Bluetooth gamepad connections disconnected\n");
}

void btc_print_status(void)
{
    printf("BT  : %s%s%s\n",
           cfg_get_bt() ? "On" : "Off",
           btc_pairing ? ", Pairing" : "",
           btc_num_connected() ? ", Connected" : "");
}

#endif /* RP6502_RIA_W */
