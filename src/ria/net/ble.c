/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "btstack.h"
#include "ble/gatt-service/hids_client.h"
#include "net/ble.h"

#if !defined(RP6502_RIA_W) || !defined(ENABLE_BLE)
void ble_task(void) {}
void ble_shutdown(void) {}
void ble_print_status(void) {}
void ble_set_config(uint8_t) {}
#else

#define DEBUG_RIA_NET_BLE

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_BLE)
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

// BLE HID connection tracking

// We can use the same indexing as hid and xin and btc so long as we keep clear
// TODO keep for later
static uint8_t ble_cid_to_pad_idx(int cid)
{
    return CFG_TUH_HID + PAD_MAX_PLAYERS + MAX_NR_HCI_CONNECTIONS + cid;
}

static bool ble_initialized;
static bool ble_pairing;

// BTStack state - BLE Central and HIDS Client
static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

// Storage for HID descriptors - BLE only
static uint8_t hid_descriptor_storage[512]; // HID descriptor storage

// We have to pause scanning during gap_connect.
// This seems the only reliable way to keep scanning.
#define BLE_SCAN_PAUSE_MS (10 * 1000)
#define BLE_CONNECT_TIMEOUT_MS (30 * 1000)
absolute_time_t ble_scan_paused_until;

void ble_start_scan(void)
{
    // if (ble_scan_paused_until)
    //     ble_scan_paused_until = make_timeout_time_ms(1000);
}

void ble_stop_scan(void)
{
    gap_stop_scan();
    ble_scan_paused_until = make_timeout_time_ms(BLE_CONNECT_TIMEOUT_MS);
}

static int ble_num_connected(void)
{
    int num = 0;
    // for (int i = 0; i < BLE_MAX_CONNECTIONS; i++)
    //     if (ble_connections[i].connected)
    //         ++num;
    return num;
}

static void ble_hids_client_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

#ifndef NDEBUG
    // HIDS client callback can receive either HCI_EVENT_PACKET or HCI_EVENT_GATTSERVICE_META directly
    if (packet_type == HCI_EVENT_PACKET)
    {
        // Standard BTstack events wrapped in HCI_EVENT_PACKET
        uint8_t event_type = hci_event_packet_get_type(packet);
        if (event_type != HCI_EVENT_GATTSERVICE_META)
        {
            DBG("BLE: HIDS client handler - unexpected event type 0x%02x in HCI packet, returning\n", event_type);
            return;
        }
    }
    else if (packet_type != HCI_EVENT_GATTSERVICE_META)
    {
        DBG("BLE: HIDS client handler - unexpected packet type 0x%02x, returning\n", packet_type);
        return;
    }
#endif

    uint8_t subevent_code = hci_event_gattservice_meta_get_subevent_code(packet);
    switch (subevent_code)
    {
    case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
    {
        uint16_t cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
        uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
        uint8_t protocol_mode = gattservice_subevent_hid_service_connected_get_protocol_mode(packet);
        uint8_t num_instances = gattservice_subevent_hid_service_connected_get_num_instances(packet);

        DBG("BLE: HID service connection result - CID: 0x%04x, Status: 0x%02x, Protocol: %d, Services: %d\n",
            cid, status, protocol_mode, num_instances);

        if (status != ERROR_CODE_SUCCESS) {
            if (status == ERROR_CODE_UNSPECIFIED_ERROR) {
                DBG("BLE: HID service connection returned ERROR_CODE_UNSPECIFIED_ERROR (0x%02x) - proceeding anyway (Xbox controller quirk)\n", status);
                // Xbox controllers often return this error but the connection works fine
            } else {
                DBG("BLE: HID service connection failed with status 0x%02x - disconnecting\n", status);
                // Connection genuinely failed, clean up
                return;
            }
        } else {
            DBG("BLE: HID service connected successfully, CID: 0x%04x\n", cid);
        }

        // Access the HID descriptor that was automatically retrieved during connection
        const uint8_t *descriptor = hids_client_descriptor_storage_get_descriptor_data(cid, 0);
        uint16_t descriptor_len = hids_client_descriptor_storage_get_descriptor_len(cid, 0);

        if (descriptor && descriptor_len > 0)
        {
            DBG("BLE: HID descriptor available (%d bytes)\n", descriptor_len);

            // Print first few bytes of descriptor for debugging
            DBG("BLE: Descriptor data: ");
            for (int i = 0; i < (descriptor_len < 16 ? descriptor_len : 16); i++)
            {
                DBG("%02x ", descriptor[i]);
            }
            DBG("\n");

            // // Enable notifications for HID input reports - this is crucial!
            // DBG("BLE: Enabling HID input report notifications...\n");
            // // ERROR_CODE_COMMAND_DISALLOWED why?
            // uint8_t enable_status = hids_client_enable_notifications(cid);
            // if (enable_status == ERROR_CODE_SUCCESS)
            // {
            //     DBG("BLE: Successfully requested HID notification enablement\n");
            // }
            // else
            // {
            //     DBG("BLE: Failed to enable HID notifications, status: 0x%02x\n", enable_status);
            // }
        }
        else
        {
            DBG("BLE: CRITICAL - No HID descriptor available! Cannot parse gamepad input without it.\n");
            return;
        }
        break;
    }

    case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
    {
        uint16_t cid = gattservice_subevent_hid_service_disconnected_get_hids_cid(packet);
        uint8_t pad_idx = ble_cid_to_pad_idx(cid);

        DBG("BLE: HID service disconnected - CID: 0x%04x, Pad Index: %d\n",
            cid, pad_idx);
        DBG("BLE: Packet details - Type: 0x%02x, Channel: 0x%04x, Size: %d\n",
            packet_type, channel, size);
        DBG("BLE: Subevent code: 0x%02x, CID from packet: 0x%04x\n",
            hci_event_gattservice_meta_get_subevent_code(packet), cid);

        // TODO: Clean up gamepad state for this CID
        // pad_disconnect(pad_idx);

        break;
    }

    case GATTSERVICE_SUBEVENT_HID_REPORT:
    {
        // Handle incoming HID reports (gamepad input)
        uint16_t cid = gattservice_subevent_hid_report_get_hids_cid(packet);
        uint8_t service_index = gattservice_subevent_hid_report_get_service_index(packet);
        uint8_t report_id = gattservice_subevent_hid_report_get_report_id(packet);
        const uint8_t *report = gattservice_subevent_hid_report_get_report(packet);
        uint16_t report_len = gattservice_subevent_hid_report_get_report_len(packet);

        static bool printed = false;
        if (!printed)
        {
            printed = true;

            DBG("BLE: Got HID report (CID: 0x%04x, service %d, id %d, %d bytes)\n",
                cid, service_index, report_id, report_len);

            // Print the raw report data for debugging
            DBG("BLE: Report data: ");
            for (int i = 0; i < report_len && i < 16; i++)
            {
                DBG("%02x ", report[i]);
            }
            DBG("\n");
        }

        // TODO: Process the HID report data for gamepad input
        // This is where you would parse the report and update gamepad state

        break;
    }
    }
}

static void ble_hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET)
        return;

    uint8_t event_type = hci_event_packet_get_type(packet);
    bd_addr_t event_addr;
    uint8_t addr_type;
    uint16_t connection_handle;

    switch (event_type)
    {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING)
        {
            DBG("BLE: Bluetooth LE Central ready and working!\n");
            gap_start_scan();
            DBG("BLE: Started scanning - ready to discover BLE gamepads\n");
        }
        break;

    case GAP_EVENT_ADVERTISING_REPORT:
    {
        gap_event_advertising_report_get_address(packet, event_addr);
        addr_type = gap_event_advertising_report_get_address_type(packet);
        uint8_t data_length = gap_event_advertising_report_get_data_length(packet);
        const uint8_t *data = gap_event_advertising_report_get_data(packet);

        // Check if this device advertises HID service (gamepad)
        if (ad_data_contains_uuid16(data_length, (uint8_t *)data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE))
        {
            // Check if we should connect based on bonding status
            // Look up device in LE device database to check if it's bonded
            bool is_bonded = false;
            for (int i = 0; i < le_device_db_max_count(); i++)
            {
                int db_addr_type = BD_ADDR_TYPE_UNKNOWN;
                bd_addr_t db_addr;
                le_device_db_info(i, &db_addr_type, db_addr, NULL);

                // Skip unused entries
                if (db_addr_type == BD_ADDR_TYPE_UNKNOWN)
                    continue;

                // Check if this entry matches our device
                if ((db_addr_type == addr_type) && (memcmp(db_addr, event_addr, 6) == 0))
                {
                    is_bonded = true;
                    break;
                }
            }

            if (!ble_pairing && !is_bonded)
            {
                DBG("BLE: Found HID device %s but pairing disabled and device not bonded - ignoring\n",
                    bd_addr_to_str(event_addr));
                return;
            }

            if (ble_pairing || is_bonded)
            {
                DBG("BLE: Found HID device %s, connecting... (bonded: %s)\n",
                    bd_addr_to_str(event_addr), is_bonded ? "yes" : "no");
                ble_stop_scan(); // Stop scanning while connecting
                gap_connect_cancel();
                gap_connect(event_addr, addr_type);
            }
        }
        break;
    }

    case HCI_EVENT_LE_META:
    {
        uint8_t subevent_code = hci_event_le_meta_get_subevent_code(packet);

        switch (subevent_code)
        {
        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
            connection_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
            hci_subevent_le_connection_complete_get_peer_address(packet, event_addr);
            addr_type = hci_subevent_le_connection_complete_get_peer_address_type(packet);
            uint8_t status = hci_subevent_le_connection_complete_get_status(packet);

            if (status == ERROR_CODE_SUCCESS)
            {
                DBG("BLE: LE Connection Complete - Handle: 0x%04x, Address: %s\n",
                    connection_handle, bd_addr_to_str(event_addr));

                // Connect to HIDS service immediately - BTStack handles the rest
                uint8_t hids_status = hids_client_connect(connection_handle, ble_hids_client_handler,
                                                          HID_PROTOCOL_MODE_REPORT, NULL);
                if (hids_status == ERROR_CODE_SUCCESS)
                {
                    DBG("BLE: HIDS client connection started successfully\n");
                }
                else
                {
                    DBG("BLE: HIDS client connection failed: 0x%02x\n", hids_status);
                }
            }
            else
            {
                DBG("BLE: LE Connection failed - Status: 0x%02x, Address: %s\n",
                    status, bd_addr_to_str(event_addr));
            }
            ble_start_scan();
            break;

        case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE:
            connection_handle = hci_subevent_le_connection_update_complete_get_connection_handle(packet);
            DBG("BLE: Connection Update Complete - Handle: 0x%04x\n", connection_handle);
            break;
        }
        break;
    }

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        connection_handle = hci_event_disconnection_complete_get_connection_handle(packet);
        DBG("BLE: Disconnection Complete - Handle: 0x%04x\n", connection_handle);
        // BTStack handles cleanup automatically
        break;
    }
}

static void ble_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type)
    {
    case SM_EVENT_JUST_WORKS_REQUEST:
        DBG("BLE: SM Just Works Request\n");
        if (ble_pairing)
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
        else
            sm_bonding_decline(sm_event_just_works_request_get_handle(packet));
        break;

    case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
        DBG("BLE: SM Numeric Comparison Request\n");
        if (ble_pairing)
            sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
        else
            sm_bonding_decline(sm_event_just_works_request_get_handle(packet));
        break;

    case SM_EVENT_AUTHORIZATION_REQUEST:
        DBG("BLE: SM Authorization Request\n");
        if (ble_pairing)
            sm_authorization_grant(sm_event_authorization_request_get_handle(packet));
        else
            sm_bonding_decline(sm_event_just_works_request_get_handle(packet));
        break;

    case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
        DBG("BLE: SM Passkey Display: %06lu\n",
            (unsigned long)sm_event_passkey_display_number_get_passkey(packet));
        break;

    case SM_EVENT_PAIRING_COMPLETE:
        DBG("BLE: SM Pairing Complete - Status: 0x%02x\n",
            sm_event_pairing_complete_get_status(packet));
        if (sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS)
        {
            DBG("BLE: Bonding successful - bonding information stored\n");
            // ble_pairing = false; // Reset bonding flag after successful bonding
        }
        else
        {
            DBG("BLE: Pairing failed - reason: 0x%02x\n",
                sm_event_pairing_complete_get_reason(packet));
        }
        break;

    case SM_EVENT_PAIRING_STARTED:
        DBG("BLE: SM Pairing Started\n");
        break;
    }
}

static void ble_init_stack(void)
{
    // Initialize L2CAP
    l2cap_init();

    // Initialize Security Manager for BLE pairing
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    // Require bonding and secure connections for all devices
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);

    // Initialize GATT Client
    gatt_client_init();

    // Initialize HID over GATT Client with descriptor storage
    hids_client_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));

    // Register for HCI events
    hci_event_callback_registration.callback = &ble_hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // Register for SM events
    sm_event_callback_registration.callback = &ble_sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);

    // Start the Bluetooth stack
    hci_power_control(HCI_POWER_ON);

    DBG("BLE: Initialized with HIDS client\n");
}

void ble_task(void)
{
    if (!ble_initialized && cyw_ready() && cfg_get_bt())
    {
        ble_init_stack();
        ble_initialized = true;
        return;
    }

    if (ble_scan_paused_until && absolute_time_diff_us(get_absolute_time(), ble_scan_paused_until) < 0)
    {
        gap_start_scan();
        ble_scan_paused_until = 0;
    }
}

void ble_set_config(uint8_t bt)
{
    if (bt == 0)
    {
        ble_shutdown();
        return;
    }

    if (bt == 2)
    {
        DBG("BLE: Enabling bonding mode - new devices can now bond\n");
        ble_pairing = true;
    }
    else
    {
        DBG("BLE: Disabling bonding mode - preventing new device bonding\n");
        ble_pairing = false;
    }
}

void ble_shutdown(void)
{
    if (ble_initialized)
    {
        // TODO: Disconnect all active connections

        hci_power_control(HCI_POWER_OFF);

        DBG("BLE: Shutdown complete - stopped scanning\n");
    }

    ble_initialized = false;
}

void ble_print_status(void)
{
    printf("BLE : %s%s\n",
           cfg_get_bt() ? "On" : "Off",
           ble_pairing ? ", Pairing" : "");
}

#endif /* RP6502_RIA_W && ENABLE_BLE */
