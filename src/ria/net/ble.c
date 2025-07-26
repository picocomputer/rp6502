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

    uint8_t subevent_code;

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
        subevent_code = hci_event_gattservice_meta_get_subevent_code(packet);
    }
    else if (packet_type == HCI_EVENT_GATTSERVICE_META)
    {
        // HID reports come directly as HCI_EVENT_GATTSERVICE_META (0xf2)
        subevent_code = hci_event_gattservice_meta_get_subevent_code(packet);
    }
    else
    {
        DBG("BLE: HIDS client handler - unexpected packet type 0x%02x, returning\n", packet_type);
        return;
    }

    switch (subevent_code)
    {
    case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
    {
        uint16_t cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
        uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
        if (status == ERROR_CODE_SUCCESS)
        {
            DBG("BLE: HID service connected successfully, CID: 0x%04x\n", cid);

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

                // Enable notifications for HID input reports - this is crucial!
                DBG("BLE: Enabling HID input report notifications...\n");
                uint8_t enable_status = hids_client_enable_notifications(cid);
                if (enable_status == ERROR_CODE_SUCCESS)
                {
                    DBG("BLE: Successfully requested HID notification enablement\n");
                }
                else
                {
                    DBG("BLE: Failed to enable HID notifications, status: 0x%02x\n", enable_status);
                }
            }
            else
            {
                DBG("BLE: No HID descriptor available yet\n");
            }
        }
        else
        {
            DBG("BLE: HID service connection failed, CID: 0x%04x, status: 0x%02x\n", cid, status);
        }
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

    case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
    {
        uint16_t cid = gattservice_subevent_hid_service_disconnected_get_hids_cid(packet);
        DBG("BLE: HID service disconnected, CID: 0x%04x\n", cid);
        break;
    }

    case GATTSERVICE_SUBEVENT_HID_REPORT_WRITTEN:
    {
        uint16_t cid = gattservice_subevent_hid_report_written_get_hids_cid(packet);
        DBG("BLE: HID report written, CID: 0x%04x\n", cid);
        break;
    }

    case GATTSERVICE_SUBEVENT_HID_INFORMATION:
    {
        uint16_t cid = gattservice_subevent_hid_information_get_hids_cid(packet);
        DBG("BLE: HID information received, CID: 0x%04x\n", cid);
        break;
    }

    default:
        DBG("BLE: Unhandled HIDS client event: 0x%02x\n", subevent_code);
        break;
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
            gap_set_scan_parameters(0, 0x0030, 0x0030);
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
            DBG("BLE: Found HID device, connecting...\n");
            gap_stop_scan(); // Stop scanning while connecting
            gap_connect(event_addr, addr_type);
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

            DBG("BLE: LE Connection Complete - Handle: 0x%04x, Address: %s\n",
                connection_handle, bd_addr_to_str(event_addr));
            break;

        case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE:
            connection_handle = hci_subevent_le_connection_update_complete_get_connection_handle(packet);
            DBG("BLE: Connection Update Complete - Handle: 0x%04x\n", connection_handle);

            uint8_t status = hids_client_connect(connection_handle, ble_hids_client_handler,
                                                 HID_PROTOCOL_MODE_REPORT, NULL);
            if (status == ERROR_CODE_SUCCESS)
            {
                DBG("BLE: HIDS client connection started\n");
            }
            else
            {
                DBG("BLE: HIDS client connection failed: 0x%02x\n", status);
            }
            break;
        }
        break;
    }

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        connection_handle = hci_event_disconnection_complete_get_connection_handle(packet);
        DBG("BLE: Disconnection Complete - Handle: 0x%04x\n", connection_handle);

        // Restart scanning to discover more gamepads
        gap_start_scan();
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
        break;

    case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
        DBG("BLE: SM Numeric Comparison Request\n");
        if (ble_pairing)
            sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
        break;

    case SM_EVENT_AUTHORIZATION_REQUEST:
        DBG("BLE: SM Authorization Request\n");
        if (!ble_pairing)
        {
            DBG("BLE: Declining authorization - not in pairing mode\n");
            sm_bonding_decline(sm_event_authorization_request_get_handle(packet));
        }
        break;

    case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
        DBG("BLE: SM Passkey Display: %06lu\n",
            (unsigned long)sm_event_passkey_display_number_get_passkey(packet));
        break;

    case SM_EVENT_PAIRING_COMPLETE:
        DBG("BLE: SM Pairing Complete - Status: 0x%02x\n",
            sm_event_pairing_complete_get_status(packet));
        break;

    default:
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
}

void ble_set_config(uint8_t bt)
{
    if (bt == 0)
    {
        ble_shutdown();
        return;
    }

    if (bt == 2)
        ble_pairing = true;
    else
        ble_pairing = false;
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
    printf("BLE : %s%s%s\n",
           cfg_get_bt() ? "On" : "Off",
           cfg_get_bt() == 2 ? ", Pairing" : "",
           cfg_get_bt() == 3 ? ", Connected" : "");
}

#endif /* RP6502_RIA_W && ENABLE_BLE */
