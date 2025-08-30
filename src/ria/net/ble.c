/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <btstack.h>

#if !defined(RP6502_RIA_W) || !defined(ENABLE_BLE)
#include "net/ble.h"
void ble_task(void) {}
void ble_shutdown(void) {}
void ble_print_status(void) {}
void ble_set_config(uint8_t) {}
void ble_set_hid_leds(uint8_t) {}
#else

#include "hid/hid.h"
#include "hid/kbd.h"
#include "hid/mou.h"
#include "hid/pad.h"
#include "net/ble.h"
#include "net/cyw.h"
#include "sys/cfg.h"
#include "sys/led.h"
#include <stdio.h>
#include <pico/time.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_BLE)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static bool ble_initialized;
static bool ble_pairing;
static uint8_t ble_count_kbd;
static uint8_t ble_count_mou;
static uint8_t ble_count_pad;

// BTStack state - BLE Central and HIDS Client
static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

// Enough storage for MAX_NR_HIDS_CLIENTS HID descriptors
// Since we only negotiate one connection at a time and only
// need the descriptor once, this could be hacked smaller.
static uint8_t hid_descriptor_storage[3 * 1024];

// We pause scanning during the entire connect sequence
// because BTStack only has state to manage one at a time.
// We peek at the in progress connection to monitor failure.
#define BLE_CONNECT_TIMEOUT_MS (20 * 1000)
static absolute_time_t ble_scan_restarts_at;
static hci_con_handle_t ble_hci_con_handle_in_progress;

static inline void ble_restart_scan(void)
{
    ble_hci_con_handle_in_progress = HCI_CON_HANDLE_INVALID;
    // If pending, fire it off immediately.
    if (ble_scan_restarts_at)
        ble_scan_restarts_at = get_absolute_time();
}

void ble_set_hid_leds(uint8_t leds)
{
    (void)leds;
    // TODO I don't have a keyboard to test this
}

static inline int ble_hids_cid_to_hid_slot(uint16_t hids_cid)
{
    return HID_BLE_START + hids_cid;
}

static void ble_hids_client_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)packet_type;
    (void)channel;
    (void)size;
    uint8_t subevent_code = hci_event_gattservice_meta_get_subevent_code(packet);
    switch (subevent_code)
    {
    case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
    {
        DBG("BLE: GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED\n");
        ble_restart_scan();
        uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
        uint16_t cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
        if (status != ERROR_CODE_SUCCESS)
        {
            // Sometimes BTstack will send us failures as the sm layer is failing.
            DBG("BLE: HID service connection failed - Status: 0x%02x, CID: 0x%04x\n", status, cid);
            break;
        }
        int slot = ble_hids_cid_to_hid_slot(cid);
        const uint8_t *descriptor = hids_client_descriptor_storage_get_descriptor_data(cid, 0);
        uint16_t descriptor_len = hids_client_descriptor_storage_get_descriptor_len(cid, 0);
        if (kbd_mount(slot, descriptor, descriptor_len))
            ++ble_count_kbd;
        if (mou_mount(slot, descriptor, descriptor_len))
            ++ble_count_mou;
        if (pad_mount(slot, descriptor, descriptor_len, 0, 0))
            ++ble_count_pad;
        break;
    }

    case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
    {
        uint16_t cid = gattservice_subevent_hid_service_disconnected_get_hids_cid(packet);
        DBG("BLE: HID service disconnected - CID: 0x%04x\n", cid);
        int slot = ble_hids_cid_to_hid_slot(cid);
        if (kbd_umount(slot))
            --ble_count_kbd;
        if (mou_umount(slot))
            --ble_count_mou;
        if (pad_umount(slot))
            --ble_count_pad;
        break;
    }

    case GATTSERVICE_SUBEVENT_HID_REPORT:
    {
        uint16_t cid = gattservice_subevent_hid_report_get_hids_cid(packet);
        int slot = ble_hids_cid_to_hid_slot(cid);
        const uint8_t *report = gattservice_subevent_hid_report_get_report(packet);
        uint16_t report_len = gattservice_subevent_hid_report_get_report_len(packet);
        kbd_report(slot, report, report_len);
        mou_report(slot, report, report_len);
        pad_report(slot, report, report_len);
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
            ble_restart_scan();
        }
        break;

    case GAP_EVENT_ADVERTISING_REPORT:
    {
        bd_addr_t event_addr;
        gap_event_advertising_report_get_address(packet, event_addr);
        uint8_t addr_type = gap_event_advertising_report_get_address_type(packet);

        uint8_t data_length = gap_event_advertising_report_get_data_length(packet);
        const uint8_t *data = gap_event_advertising_report_get_data(packet);

        // Skip if we're connecting
        if (ble_scan_restarts_at)
            break;

        // DBG("BLE: GAP_EVENT_ADVERTISING_REPORT %s\n", bd_addr_to_str(event_addr));

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
            if ((db_addr_type == addr_type) && !memcmp(db_addr, event_addr, 6))
            {
                is_bonded = true;
                break;
            }
        }

        if (!is_bonded)
        {
            // Don't add new devices unless pairing is active
            if (!ble_pairing)
                break;
            // Bonded devices don't always advertise HID
            // but it's required for pairing.
            if (!ad_data_contains_uuid16(data_length, data,
                                         ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE))
                break;
        }

        uint8_t connect_status = gap_connect(event_addr, addr_type);
        if (connect_status == ERROR_CODE_SUCCESS)
        {
            hci_connection_t *conn = hci_connection_for_bd_addr_and_type(event_addr, addr_type);
            if (conn)
                ble_hci_con_handle_in_progress = conn->con_handle;
            DBG("BLE: Found HID %s, connecting... (bonded: %s)\n",
                bd_addr_to_str(event_addr), is_bonded ? "yes" : "no");
            gap_stop_scan();
            ble_scan_restarts_at = make_timeout_time_ms(BLE_CONNECT_TIMEOUT_MS);
        }
        else
        {
            DBG("BLE: Found HID %s, connect failed with status 0x%02x (bonded: %s)\n",
                bd_addr_to_str(event_addr), connect_status, is_bonded ? "yes" : "no");
        }
        break;
    }

    case HCI_EVENT_LE_META:
    {
        uint8_t subevent_code = hci_event_le_meta_get_subevent_code(packet);
        switch (subevent_code)
        {
        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
            uint8_t status = hci_subevent_le_connection_complete_get_status(packet);
            if (status != ERROR_CODE_SUCCESS)
            {
                DBG("BLE: LE Connection failed - Status: 0x%02x\n", status);
                ble_restart_scan();
                break;
            }
            hci_con_handle_t hci_con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
            uint8_t hids_status = hids_client_connect(hci_con_handle, ble_hids_client_handler,
                                                      HID_PROTOCOL_MODE_REPORT,
                                                      NULL);
            if (hids_status != ERROR_CODE_SUCCESS)
            {
                DBG("BLE: HIDS client connection failed: 0x%02x\n", hids_status);
                ble_restart_scan();
            }
        }
        break;
    }

    case HCI_EVENT_DISCONNECTION_COMPLETE:
    {
        hci_con_handle_t con_handle = hci_event_disconnection_complete_get_connection_handle(packet);
        DBG("BLE: Disconnection Complete - Handle: 0x%04x\n", con_handle);
        // New connection disconnected before success or timeout
        if (ble_hci_con_handle_in_progress == con_handle)
            ble_restart_scan();
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
            sm_bonding_decline(sm_event_numeric_comparison_request_get_handle(packet));
        break;

    case SM_EVENT_AUTHORIZATION_REQUEST:
        DBG("BLE: SM Authorization Request\n");
        if (ble_pairing)
            sm_authorization_grant(sm_event_authorization_request_get_handle(packet));
        else
            sm_bonding_decline(sm_event_authorization_request_get_handle(packet));
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
            led_blink(false);
        }
        else
        {
            DBG("BLE: Pairing failed - reason: 0x%02x\n",
                sm_event_pairing_complete_get_reason(packet));
        }
        break;

    case SM_EVENT_REENCRYPTION_COMPLETE:
    {
        hci_con_handle_t handle = sm_event_reencryption_complete_get_handle(packet);
        uint8_t status = sm_event_reencryption_complete_get_status(packet);
        if (status != ERROR_CODE_SUCCESS)
        {
            DBG("BLE: Re-encryption failed with status 0x%02x\n", status);
            if (status == ERROR_CODE_PIN_OR_KEY_MISSING)
            {
                DBG("BLE: 0x06 PIN_OR_KEY_MISSING - deleting bond\n");
                bd_addr_t addr;
                uint8_t addr_type = sm_event_reencryption_complete_get_addr_type(packet);
                sm_event_reencryption_complete_get_address(packet, addr);
                gap_delete_bonding(addr_type, addr);
            }
            gap_disconnect(handle);
        }
        break;
    }
    }
}

static void ble_init_stack(void)
{
    // Globals
    ble_scan_restarts_at = 0;
    ble_hci_con_handle_in_progress = HCI_CON_HANDLE_INVALID;
    ble_count_kbd = 0;
    ble_count_mou = 0;
    ble_count_pad = 0;

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
    memset(hid_descriptor_storage, 0, sizeof(hid_descriptor_storage));
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
    if (!ble_initialized)
    {
        if (cfg_get_rf() && cfg_get_ble())
        {
            ble_init_stack();
            ble_initialized = true;
            ble_scan_restarts_at = make_timeout_time_ms(BLE_CONNECT_TIMEOUT_MS);
        }
        return;
    }

    if (ble_scan_restarts_at &&
        absolute_time_diff_us(get_absolute_time(), ble_scan_restarts_at) < 0)
    {
        ble_scan_restarts_at = 0;
        gap_connect_cancel();
        gap_start_scan();
        DBG("BLE: restarting gap_start_scan\n");
    }
}

void ble_set_config(uint8_t ble)
{
    switch (ble)
    {
    case 0:
        ble_shutdown();
        break;
    case 1:
        ble_pairing = false;
        led_blink(false);
        break;
    case 2:
        if (cfg_get_rf())
        {
            ble_pairing = true;
            led_blink(true);
        }
        break;
    }
}

bool ble_is_pairing(void)
{
    return ble_pairing;
}

void ble_shutdown(void)
{
    ble_pairing = false;
    led_blink(false);
    if (ble_initialized)
    {
        hci_disconnect_all();
        gap_stop_scan();
        gap_connect_cancel();
        hci_power_control(HCI_POWER_OFF);
        hci_remove_event_handler(&hci_event_callback_registration);
        sm_remove_event_handler(&sm_event_callback_registration);
        hids_client_deinit();
        sm_deinit();
        l2cap_deinit();
        att_server_deinit();
        btstack_memory_deinit();
        btstack_crypto_deinit(); // OMG! This was so hard to find.
    }
    ble_initialized = false;
}

void ble_print_status(void)
{
    if (cfg_get_ble())
    {
        if (cfg_get_rf())
            printf("BLE : %d keyboard%s, %d %s, %d gamepad%s%s\n",
                   ble_count_kbd, ble_count_kbd == 1 ? "" : "s",
                   ble_count_mou, ble_count_mou == 1 ? "mouse" : "mice",
                   ble_count_pad, ble_count_pad == 1 ? "" : "s",
                   ble_pairing ? ", pairing" : "");
        else
            printf("BLE : radio off\n");
    }
    else
    {
        printf("BLE : disabled\n");
    }
}

#endif /* RP6502_RIA_W && ENABLE_BLE */
