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
    bd_addr_t remote_addr;
    uint8_t addr_type;
    uint16_t connection_handle;
    uint16_t hids_cid;
    bool connected;
    bool hid_service_found;
    absolute_time_t connection_timeout;
} ble_connection_t;

static ble_connection_t ble_connections[BLE_MAX_CONNECTIONS];
static bool ble_initialized;
static bool ble_scanning;
static bool ble_pairing;
static absolute_time_t ble_next_scan;

// BTStack state - BLE Central and GATT Client
static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

// Storage for HID descriptors - BLE only
static uint8_t hid_descriptor_storage[512]; // HID descriptor storage

static int find_connection_by_handle(uint16_t connection_handle)
{
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++)
        if (ble_connections[i].connected && ble_connections[i].connection_handle == connection_handle)
            return i;
    return -1;
}

static int create_connection_entry(bd_addr_t addr, uint8_t addr_type, uint16_t connection_handle)
{
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++)
    {
        if (!ble_connections[i].connected)
        {
            memcpy(ble_connections[i].remote_addr, addr, BD_ADDR_LEN);
            ble_connections[i].addr_type = addr_type;
            ble_connections[i].connection_handle = connection_handle;
            ble_connections[i].connected = true;
            ble_connections[i].hid_service_found = false;
            ble_connections[i].hids_cid = 0;
            ble_connections[i].connection_timeout = make_timeout_time_ms(BLE_CONNECTION_TIMEOUT_SECS * 1000);
            return i;
        }
    }
    return -1;
}

static void remove_connection_entry(int slot)
{
    if (slot >= 0 && slot < BLE_MAX_CONNECTIONS)
    {
        if (ble_connections[slot].connected)
        {
            pad_umount(ble_slot_to_pad_idx(slot));
            memset(&ble_connections[slot], 0, sizeof(ble_connection_t));
            DBG("BLE: Removed connection slot %d\n", slot);
        }
    }
}

static int ble_num_connected(void)
{
    int num = 0;
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++)
        if (ble_connections[i].connected)
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
        if (ble_connections[i].connected && ble_connections[i].hid_service_found)
        {
            // For now, just report empty data - proper implementation would parse packet
            uint8_t dummy_report = 0;
            pad_report(ble_slot_to_pad_idx(i), &dummy_report, 1);
            break; // For now, just use first connected device
        }
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
            gap_set_scan_parameters(0, 0x0030, 0x0030); // Set scan parameters
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

        DBG("BLE: Advertisement from %s (type: %d, RSSI: %d dBm)\n",
            bd_addr_to_str(event_addr), addr_type, rssi);

        // Look for HID service UUID in advertisement data
        bool has_hid_service = false;
        ad_context_t context;
        for (ad_iterator_init(&context, data_length, data); ad_iterator_has_more(&context); ad_iterator_next(&context))
        {
            uint8_t data_type = ad_iterator_get_data_type(&context);
            uint8_t data_size = ad_iterator_get_data_len(&context);
            const uint8_t *ad_data = ad_iterator_get_data(&context);

            // Check for 16-bit Service UUIDs (0x1812 = HID Service)
            if (data_type == BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS ||
                data_type == BLUETOOTH_DATA_TYPE_INCOMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS)
            {
                for (int i = 0; i < data_size; i += 2)
                {
                    uint16_t uuid = little_endian_read_16(ad_data, i);
                    if (uuid == ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE)
                    {
                        has_hid_service = true;
                        break;
                    }
                }
            }
        }

        // Connect to devices advertising HID service during pairing mode
        if (has_hid_service && ble_pairing && ble_num_connected() < BLE_MAX_CONNECTIONS)
        {
            DBG("BLE: Found HID device, attempting connection...\n");
            gap_stop_scan();
            ble_scanning = false;
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

            int slot = create_connection_entry(event_addr, addr_type, connection_handle);
            if (slot >= 0)
            {
                DBG("BLE: Created connection slot %d\n", slot);
                // Start service discovery for HID service
                gatt_client_discover_primary_services_by_uuid16(ble_hid_report_handler,
                                                                connection_handle,
                                                                ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
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

        int slot = find_connection_by_handle(connection_handle);
        if (slot >= 0)
            remove_connection_entry(slot);
        break;

    case GATT_EVENT_SERVICE_QUERY_RESULT:
    {
        connection_handle = gatt_event_service_query_result_get_handle(packet);
        gatt_client_service_t service;
        gatt_event_service_query_result_get_service(packet, &service);

        DBG("BLE: HID Service found - Handle: 0x%04x, Start: 0x%04x, End: 0x%04x\n",
            connection_handle, service.start_group_handle, service.end_group_handle);

        slot = find_connection_by_handle(connection_handle);
        if (slot >= 0)
        {
            ble_connections[slot].hid_service_found = true;
            // For simplicity, assume successful mount - in real implementation,
            // would discover characteristics and enable notifications
            bool mounted = pad_mount(ble_slot_to_pad_idx(slot), NULL, 0, 0, 0, 0);
            if (mounted)
            {
                ble_pairing = false;
                DBG("BLE: *** HID GAMEPAD CONFIRMED! *** Successfully mounted at slot %d\n", slot);
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
        {
            slot = find_connection_by_handle(connection_handle);
            if (slot >= 0 && !ble_connections[slot].hid_service_found)
            {
                DBG("BLE: HID service not found, disconnecting...\n");
                gap_disconnect(connection_handle);
            }
        }
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

    // Handle periodic scanning while in pairing mode
    if (ble_initialized && ble_pairing && !ble_scanning)
    {
        if (absolute_time_diff_us(ble_next_scan, get_absolute_time()) > 0)
        {
            DBG("BLE: Starting scan for HID devices...\n");
            gap_set_scan_parameters(0, 0x0030, 0x0030); // Active scanning
            gap_start_scan();
            ble_scanning = true;
            ble_next_scan = make_timeout_time_ms(10000); // 10 seconds
        }
    }

    // Check for connection timeouts
    if (ble_initialized)
    {
        absolute_time_t now = get_absolute_time();
        for (int i = 0; i < BLE_MAX_CONNECTIONS; i++)
        {
            if (ble_connections[i].connected &&
                !ble_connections[i].hid_service_found &&
                absolute_time_diff_us(ble_connections[i].connection_timeout, now) > 0)
            {
                DBG("BLE: Connection timeout for slot %d\n", i);
                gap_disconnect(ble_connections[i].connection_handle);
            }
        }
    }
}

void ble_set_config(uint8_t bt)
{
    if (bt == 0)
        ble_shutdown();
    else if (bt == 2 && ble_num_connected() < BLE_MAX_CONNECTIONS)
        ble_pairing = true;
    else
        ble_pairing = false;

    if (ble_pairing && ble_scanning)
    {
        gap_stop_scan();
        ble_scanning = false;
    }
}

void ble_shutdown(void)
{
    if (ble_initialized)
    {
        if (ble_scanning)
        {
            gap_stop_scan();
            ble_scanning = false;
        }

        // Disconnect all active connections
        for (int i = 0; i < BLE_MAX_CONNECTIONS; i++)
        {
            if (ble_connections[i].connected)
                gap_disconnect(ble_connections[i].connection_handle);
        }

        hci_power_control(HCI_POWER_OFF);
    }

    ble_initialized = false;
    ble_pairing = false;
    memset(ble_connections, 0, sizeof(ble_connections));
    DBG("BLE: All Bluetooth LE gamepad connections disconnected\n");
}

void ble_print_status(void)
{
    printf("BLE : %s%s%s\n",
           cfg_get_bt() ? "On" : "Off",
           ble_pairing ? ", Pairing" : "",
           ble_num_connected() ? ", Connected" : "");
}

#endif /* RP6502_RIA_W && ENABLE_BLE */
