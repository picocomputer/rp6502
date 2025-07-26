/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "btstack.h"
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

// We can use the same indexing as hid and xin and btc so long as we keep clear
static uint8_t ble_slot_to_pad_idx(int slot)
{
    return CFG_TUH_HID + PAD_MAX_PLAYERS + MAX_NR_HCI_CONNECTIONS + slot;
}

// Connection tracking for BLE HID devices
#define BLE_CONNECTION_TIMEOUT_SECS 10
#define BLE_MAX_CONNECTIONS 4
typedef struct
{
    uint8_t unknown; // TODO
    // bd_addr_t remote_addr;
    // uint16_t connection_handle;
    // bool connected;
    // bool hid_service_found;
} ble_connection_t;

static ble_connection_t ble_connections[BLE_MAX_CONNECTIONS];
static bool ble_initialized;

// BTStack state - BLE Central and GATT Client
static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

// Storage for HID descriptors - BLE only
static uint8_t hid_descriptor_storage[512]; // HID descriptor storage

static int ble_num_connected(void)
{
    int num = 0;
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++)
        if (ble_connections[i].unknown)
            ++num;
    return num;
}

static void ble_hid_report_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(packet_type);
    UNUSED(channel);

    if (size < 4)
        return; // Need at least handle + data

    UNUSED(packet); // Remove unused variable warnings for now

    // Find connection by GATT handle (simplified - would need proper handle mapping)
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++)
    {
        // TODO
    }
}

static void ble_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
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

            // Set up scanning parameters - always ready to discover BLE gamepads
            gap_set_scan_parameters(0, 0x0030, 0x0030);

            // Start scanning by default so we can always discover BLE gamepads
            gap_start_scan();

            DBG("BLE: Started scanning - ready to discover BLE gamepads\n");
        }
        break;
    case GAP_EVENT_ADVERTISING_REPORT:
    {
        gap_event_advertising_report_get_address(packet, event_addr);
        addr_type = gap_event_advertising_report_get_address_type(packet);
        UNUSED(gap_event_advertising_report_get_advertising_event_type(packet)); // Suppress unused warning
        uint8_t rssi = gap_event_advertising_report_get_rssi(packet);
        uint8_t data_length = gap_event_advertising_report_get_data_length(packet);
        const uint8_t *data = gap_event_advertising_report_get_data(packet);

        // DBG("BLE: Advertisement from %s (type: %d, RSSI: %d dBm)\n",
        //     bd_addr_to_str(event_addr), addr_type, rssi);

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

            // Restart scanning to discover more BLE gamepads
            gap_start_scan();

            // Check if GATT client is ready before discovering services
            if (gatt_client_is_ready(connection_handle))
            {
                uint8_t status = gatt_client_discover_primary_services_by_uuid16(ble_hid_report_handler,
                                                                                 connection_handle,
                                                                                 ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
                if (status != ERROR_CODE_SUCCESS)
                {
                    DBG("BLE: Failed to start service discovery, status: 0x%02x\n", status);
                }
            }
            else
            {
                DBG("BLE: GATT client not ready for connection handle 0x%04x\n", connection_handle);
            }
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

        // BLE gamepad disconnected - restart scanning to discover more gamepads
        gap_start_scan();
        break;

    case GATT_EVENT_SERVICE_QUERY_RESULT:
    {
        connection_handle = gatt_event_service_query_result_get_handle(packet);
        gatt_client_service_t service;
        gatt_event_service_query_result_get_service(packet, &service);

        DBG("BLE: HID Service found - Handle: 0x%04x, discovering characteristics...\n", connection_handle);

        gatt_client_discover_characteristics_for_service(ble_hid_report_handler,
                                                         connection_handle, &service);
        break;
    }

    case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
    {
        connection_handle = gatt_event_characteristic_query_result_get_handle(packet);
        gatt_client_characteristic_t characteristic;
        gatt_event_characteristic_query_result_get_characteristic(packet, &characteristic);

        // Check if this is the Report Map characteristic (0x2A4B)
        if (characteristic.uuid16 == ORG_BLUETOOTH_CHARACTERISTIC_REPORT_MAP)
        {
            DBG("BLE: Found Report Map characteristic, reading HID descriptor...\n");
            // Read the HID descriptor
            gatt_client_read_value_of_characteristic(ble_hid_report_handler,
                                                     connection_handle, &characteristic);
        }
        break;
    }

    case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT:
    {
        connection_handle = gatt_event_characteristic_value_query_result_get_handle(packet);
        uint16_t value_length = gatt_event_characteristic_value_query_result_get_value_length(packet);
        const uint8_t *value = gatt_event_characteristic_value_query_result_get_value(packet);

        DBG("BLE: Got HID descriptor (%d bytes), checking if it's a gamepad...\n", value_length);

        int slot = 0; //////// TODO
        if (slot >= 0)
        {
            // Try to mount with the actual HID descriptor
            bool mounted = pad_mount(ble_slot_to_pad_idx(slot), value, value_length, 0, 0, 0);
            if (mounted)
            {
                DBG("BLE: *** HID GAMEPAD CONFIRMED! *** Successfully mounted at slot %d\n", slot);
            }
            else
            {
                DBG("BLE: HID descriptor indicates non-gamepad device, disconnecting...\n");
                gap_disconnect(connection_handle);
            }
        }
        break;
    }

    case GATT_EVENT_QUERY_COMPLETE:
        connection_handle = gatt_event_query_complete_get_handle(packet);
        uint8_t att_status = gatt_event_query_complete_get_att_status(packet);

        DBG("BLE: GATT Query Complete - Handle: 0x%04x, Status: 0x%02x\n",
            connection_handle, att_status);

        if (att_status != ATT_ERROR_SUCCESS)
            gap_disconnect(connection_handle);
        break;

    default:
        // DBG("BLE: Unhandled HCI event 0x%02x\n", event_type);
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
        sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
        break;

    case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
        DBG("BLE: SM Numeric Comparison Request\n");
        sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
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
    // Clear connection array
    memset(ble_connections, 0, sizeof(ble_connections));

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
    hci_event_callback_registration.callback = &ble_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // Register for SM events
    sm_event_callback_registration.callback = &ble_sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);

    // Start the Bluetooth stack
    hci_power_control(HCI_POWER_ON);

    DBG("BLE: Initialized\n");
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

    // BLE stack should always be initialized to scan for and connect to gamepads
    if (!ble_initialized && cyw_ready())
    {
        ble_init_stack();
        ble_initialized = true;
    }

    if (!ble_initialized)
        return;

    if (bt == 2)
    {
        // Pairing mode - actively scan for BLE gamepads
        gap_start_scan();
        DBG("BLE: Pairing mode enabled - actively scanning for BLE gamepads\n");
    }
    else if (bt == 1)
    {
        // Normal mode - still scan but maybe less aggressively (for now, same as pairing mode)
        gap_start_scan();
        DBG("BLE: Normal mode - scanning for BLE gamepads\n");
    }
}
void ble_shutdown(void)
{
    if (ble_initialized)
    {
        // Stop scanning for BLE gamepads
        gap_stop_scan();

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
           ble_num_connected() ? ", Connected" : "");
}

#endif /* RP6502_RIA_W && ENABLE_BLE */
