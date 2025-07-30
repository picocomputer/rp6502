/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "btstack.h"
#include "ble/gatt-service/hids_client.h"
#include "net/ble.h"
#include "usb/hid.h"

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
#include "hid/kbd.h"
#include "hid/mou.h"
#include "hid/pad.h"
#include <stdio.h>
#include <string.h>
#include "pico/time.h"

#define BLE_APPEARANCE_KBD 0x03C1
#define BLE_APPEARANCE_MOU 0x03C2
#define BLE_APPEARANCE_PAD 0x03C4

// BLE HID connection tracking
typedef struct
{
    bd_addr_type_t addr_type;
    bd_addr_t addr;
    hci_con_handle_t hci_con_handle;
    uint16_t hids_cid;
    uint16_t appearance;
} ble_connection_t;

ble_connection_t ble_connections[MAX_NR_HCI_CONNECTIONS];

static bool ble_initialized;
static bool ble_pairing;

// BTStack state - BLE Central and HIDS Client
static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

// Storage for HID descriptors - BLE only
static uint8_t hid_descriptor_storage[512]; // HID descriptor storage

// We pause scanning during the entire connect sequence
// becase BTStack only has state to manage one at a time.
// We peek at the newest connection to restart scan.
#define BLE_CONNECT_TIMEOUT_MS (30 * 1000)
absolute_time_t ble_scan_restarts_at;
ble_connection_t *ble_newest_connection;

static inline void ble_restart_scan(void)
{
    // If pending, fire it off immediately.
    if (ble_scan_restarts_at)
        ble_scan_restarts_at = get_absolute_time();
}

static int ble_num_connected(void)
{
    int num = 0;
    for (int i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
        if (ble_connections[i].hids_cid)
            ++num;
    return num;
}

static uint8_t ble_idx_to_hid_slot(int idx)
{
    // We can use the same indexing as hid and xin so long as we keep clear
    return HID_BLE_START + idx;
}

static void ble_clear_index(int index)
{
    if (index < 0)
        return;
    ble_connections[index].addr_type = BD_ADDR_TYPE_UNKNOWN;
    ble_connections[index].hci_con_handle = HCI_CON_HANDLE_INVALID;
    ble_connections[index].hids_cid = 0; // cids start at 1
    ble_connections[index].appearance = 0;
}

static int ble_get_empty_index(void)
{
    for (uint8_t i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
        if (ble_connections[i].addr_type == BD_ADDR_TYPE_UNKNOWN)
            return i;
    return -1; // Not found
}

static int ble_get_index_by_addr(bd_addr_type_t addr_type, const uint8_t *addr)
{
    for (uint8_t i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
        if (ble_connections[i].addr_type == addr_type &&
            memcmp(ble_connections[i].addr, addr, 6) == 0)
            return i;
    return -1; // Not found
}

static int ble_get_index_by_handle(hci_con_handle_t handle)
{
    for (uint8_t i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
        if (ble_connections[i].hci_con_handle == handle)
            return i;
    return -1; // Not found
}

static int ble_get_index_by_cid(uint16_t hids_cid)
{
    for (uint8_t i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
    {
        if (ble_connections[i].hids_cid == hids_cid)
        {
            return i;
        }
    }
    return -1; // Not found
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

        ble_restart_scan();

        DBG("BLE: HID service connection result - CID: 0x%04x, Status: 0x%02x, Protocol: %d, Services: %d\n",
            cid, status, protocol_mode, num_instances);

        int index = ble_get_index_by_cid(cid);
        if (index < 0)
        {
            DBG("BLE: ble_get_index_by_cid failed\n");
            break;
        }

        if (status != ERROR_CODE_SUCCESS)
        {
            DBG("BLE: HID service connection failed with status 0x%02x\n", status);
            if (ble_connections[index].hci_con_handle != HCI_CON_HANDLE_INVALID)
            {
                DBG("BLE: Disconnecting HCI connection handle 0x%04x due to HID service failure\n",
                    ble_connections[index].hci_con_handle);
                gap_disconnect(ble_connections[index].hci_con_handle);
            }
            if (status == ERROR_CODE_UNSPECIFIED_ERROR)
            {
                // XBox gamepads end up here when the bonding is invalid/old/whatevs
                // TODO is there a way to catch this on hci layer?
                DBG("BLE: ERROR_CODE_UNSPECIFIED_ERROR deleting bonding\n");
                gap_delete_bonding(ble_connections[index].addr_type, ble_connections[index].addr);
            }
            break;
        }

        // Access the HID descriptor that was automatically retrieved during connection
        const uint8_t *descriptor = hids_client_descriptor_storage_get_descriptor_data(cid, 0);
        uint16_t descriptor_len = hids_client_descriptor_storage_get_descriptor_len(cid, 0);

        if (!descriptor || descriptor_len <= 0)
        {
            DBG("BLE: No HID descriptor available!\n");
            break;
        }

        DBG("BLE: HID descriptor available (%d bytes)\n", descriptor_len);

        // Print first few bytes of descriptor for debugging
        DBG("BLE: Descriptor data: ");
        for (int i = 0; i < (descriptor_len < 16 ? descriptor_len : 16); i++)
        {
            DBG("%02x ", descriptor[i]);
        }
        DBG("\n");

        bool mounted = pad_mount(ble_idx_to_hid_slot(index), descriptor, descriptor_len, 0, 0);
        if (mounted)
        {
            DBG("BLE: gamepad mounted player %d\n",
                pad_get_player_num(ble_idx_to_hid_slot(index)));
        }

        mounted = mou_mount(ble_idx_to_hid_slot(index), descriptor, descriptor_len);
        if (mounted)
        {
            DBG("BLE: mouse mounted\n");
        }
        break;
    }

    case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
    {
        uint16_t cid = gattservice_subevent_hid_service_disconnected_get_hids_cid(packet);
        DBG("BLE: HID service disconnected - CID: 0x%04x\n", cid);
        int index = ble_get_index_by_cid(cid);
        if (index >= 0)
            pad_umount(ble_idx_to_hid_slot(index));
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

        int index = ble_get_index_by_cid(cid);
        if (index < 0)
        {
            DBG("BLE: ble_get_index_by_cid failed\n");
            break;
        }

        // kbd_report(ble_idx_to_hid_slot(index), report, report_len);
        mou_report(ble_idx_to_hid_slot(index), report, report_len);
        pad_report(ble_idx_to_hid_slot(index), report, report_len);

        // switch (ble_connections[index].appearance)
        // {
        // case BLE_APPEARANCE_KBD:
        //     kbd_report(ble_idx_to_hid_slot(index), report, report_len);
        //     break;
        // case BLE_APPEARANCE_MOU:
        //     mou_report_boot(ble_idx_to_hid_slot(index), report, report_len);
        //     break;
        // case BLE_APPEARANCE_PAD:
        // default:
        //     pad_report(ble_idx_to_hid_slot(index), report, report_len);
        //     break;
        // }

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

    switch (hci_event_packet_get_type(packet))
    {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING)
        {
            DBG("BLE: Bluetooth LE Central ready and working!\n");
            gap_start_scan();
        }
        break;

    case GAP_EVENT_ADVERTISING_REPORT:
    {
        bd_addr_t event_addr;
        gap_event_advertising_report_get_address(packet, event_addr);
        uint8_t addr_type = gap_event_advertising_report_get_address_type(packet);

        uint8_t data_length = gap_event_advertising_report_get_data_length(packet);
        const uint8_t *data = gap_event_advertising_report_get_data(packet);

        ad_context_t context;
        uint16_t appearance = 0;
        bool is_hid = false;
        for (ad_iterator_init(&context, data_length, data);
             ad_iterator_has_more(&context);
             ad_iterator_next(&context))
        {
            uint8_t data_type = ad_iterator_get_data_type(&context);
            uint8_t data_len = ad_iterator_get_data_len(&context);
            const uint8_t *ad_data = ad_iterator_get_data(&context);

            switch (data_type)
            {
            case BLUETOOTH_DATA_TYPE_INCOMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS:
            case BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS:
                for (uint8_t i = 0u; (i + 2u) <= data_len; i += 2u)
                    if (little_endian_read_16(ad_data, (int)i) == ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE)
                        is_hid = true;
                break;
            case BLUETOOTH_DATA_TYPE_INCOMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS:
            case BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS:
                uint8_t ad_uuid128[16], uuid128_bt[16];
                uuid_add_bluetooth_prefix(ad_uuid128, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
                reverse_128(ad_uuid128, uuid128_bt);
                for (uint8_t i = 0u; (i + 16u) <= data_len; i += 16u)
                    if (memcmp(uuid128_bt, &ad_data[i], 16) == 0)
                        is_hid = true;
                break;
            case BLUETOOTH_DATA_TYPE_APPEARANCE:
                if (data_len >= 2)
                    appearance = little_endian_read_16(ad_data, 0);
                break;
            default:
                break;
            }
        }

        if (!is_hid)
        {
            // DBG("BLE: device %s not HID\n", bd_addr_to_str(event_addr));
            break;
        }

        DBG("BLE: Found device %s (appearance: 0x%04X)\n",
            bd_addr_to_str(event_addr), appearance);

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
            DBG("BLE: Found HID %s but new pairing is disabled\n", bd_addr_to_str(event_addr));
            break;
        }

        if (ble_pairing || is_bonded)
        {
            int index = ble_get_empty_index();
            if (index < 0)
            {
                DBG("BLE: ble_get_empty_index failed\n");
                break;
            }
            ble_newest_connection = &ble_connections[index];

            uint8_t connect_status = gap_connect(event_addr, addr_type);
            if (connect_status == ERROR_CODE_SUCCESS)
            {
                gap_stop_scan();
                DBG("BLE: Found HID %s, connecting... (bonded: %s)\n",
                    bd_addr_to_str(event_addr), is_bonded ? "yes" : "no");
                ble_scan_restarts_at = make_timeout_time_ms(BLE_CONNECT_TIMEOUT_MS);
                memcpy(ble_newest_connection->addr, event_addr, sizeof(bd_addr_t));
                ble_newest_connection->addr_type = addr_type;
                ble_newest_connection->appearance = appearance;
            }
            else
            {
                DBG("BLE: Found HID %s, connect failed with status 0x%02x (bonded: %s)\n",
                    bd_addr_to_str(event_addr), connect_status, is_bonded ? "yes" : "no");
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
            bd_addr_t event_addr;
            hci_subevent_le_connection_complete_get_peer_address(packet, event_addr);
            uint8_t addr_type = hci_subevent_le_connection_complete_get_peer_address_type(packet);
            uint8_t status = hci_subevent_le_connection_complete_get_status(packet);
            hci_con_handle_t con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);

            if (status != ERROR_CODE_SUCCESS)
            {
                ble_restart_scan();
                DBG("BLE: LE Connection failed - Status: 0x%02x, Address: %s\n",
                    status, bd_addr_to_str(event_addr));
                break;
            }

            int index = ble_get_index_by_addr(addr_type, event_addr);
            if (index < 0)
            {
                DBG("BLE: ble_get_index_by_addr failed\n");
                break;
            }
            ble_connections[index].hci_con_handle = con_handle;

            hid_protocol_mode_t mode = HID_PROTOCOL_MODE_REPORT;
            // if (ble_connections[index].appearance == BLE_APPEARANCE_KBD ||
            //     ble_connections[index].appearance == BLE_APPEARANCE_MOU)
            //     mode = HID_PROTOCOL_MODE_BOOT;

            DBG("BLE: LE Connection Complete - Address: %s, %s\n",
                bd_addr_to_str(event_addr), mode == HID_PROTOCOL_MODE_BOOT ? "BOOT" : "REPORT");

            uint8_t hids_status = hids_client_connect(con_handle, ble_hids_client_handler,
                                                      mode, &ble_connections[index].hids_cid);
            if (hids_status != ERROR_CODE_SUCCESS)
                DBG("BLE: HIDS client connection failed: 0x%02x\n", hids_status);
        }
        break;
    }

    case HCI_EVENT_DISCONNECTION_COMPLETE:
    {
        hci_con_handle_t con_handle = hci_event_disconnection_complete_get_connection_handle(packet);
        DBG("BLE: Disconnection Complete - Handle: 0x%04x\n", con_handle);
        if (ble_newest_connection->hci_con_handle == con_handle)
            ble_restart_scan();
        ble_clear_index(ble_get_index_by_handle(con_handle));
        break;
    }
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
        if (sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS)
        {
            DBG("BLE: Pairing complete - disabling pairing mode\n");
            ble_pairing = false;
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
    // Globals
    ble_scan_restarts_at = 0;
    for (int i = 0; i < MAX_NR_HCI_CONNECTIONS; i++)
        ble_clear_index(i);
    ble_newest_connection = &ble_connections[0];

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
    if (!ble_initialized && cyw_ready() && cfg_get_ble())
    {
        ble_init_stack();
        ble_initialized = true;
    }

    if (ble_initialized && ble_scan_restarts_at &&
        absolute_time_diff_us(get_absolute_time(), ble_scan_restarts_at) < 0)
    {
        ble_scan_restarts_at = 0;
        gap_start_scan();
        DBG("BLE: restarting gap_start_scan\n");
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
        gap_auto_connection_stop_all();

        hci_power_control(HCI_POWER_OFF);

        DBG("BLE: Shutdown complete - stopped scanning\n");
    }

    ble_initialized = false;
}

void ble_print_status(void)
{
    printf("BLE : %s%s\n",
           cfg_get_ble() ? "On" : "Off",
           ble_pairing ? ", Pairing" : "");
}

#endif /* RP6502_RIA_W && ENABLE_BLE */
