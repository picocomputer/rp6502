/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if !defined(RP6502_RIA_W)
#include "net/ble.h"
void ble_task(void) {}
int ble_status_response(char *, size_t, int) { return -1; }
void ble_set_hid_leds(uint8_t) {}
#else

#include "hid/hid.h"
#include "hid/kbd.h"
#include "hid/mou.h"
#include "hid/pad.h"
#include "net/ble.h"
#include "net/cyw.h"
#include "str/str.h"
#include "sys/cfg.h"
#include "sys/led.h"
#include "main.h"
#include <stdio.h>
#include <pico/time.h>
#include <btstack.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_BLE)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static uint8_t ble_enabled = 1;
static bool ble_initialized;
static bool ble_shutting_down;
static bool ble_pairing;
static uint8_t ble_count_kbd;
static uint8_t ble_count_mou;
static uint8_t ble_count_pad;

// LED output report state for BLE keyboards
static absolute_time_t ble_hid_leds_at;
static uint8_t ble_hid_leds;
static uint16_t ble_kbd_cids[MAX_NR_HIDS_CLIENTS];

// BTStack state - BLE Central and HIDS Client
static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

// Enough storage for MAX_NR_HIDS_CLIENTS HID descriptors
// Since we only negotiate one connection at a time and only
// need the descriptor once, this could be hacked smaller.
static uint8_t hid_descriptor_storage[3 * 1024];

// Only one connection sequence at a time. ble_connecting_handle tracks
// the LE handle from connection complete through HIDS service setup.
// ble_scan_restarts_at schedules the next scan/whitelist attempt and
// doubles as a timeout for the in-progress connection.
#define BLE_CONNECT_TIMEOUT_MS (20 * 1000)
static absolute_time_t ble_scan_restarts_at;
static hci_con_handle_t ble_connecting_handle;

static void ble_connect_with_whitelist(void)
{
    // Add all bonded devices to whitelist
    gap_whitelist_clear();
    int added = 0;
    for (int i = 0; i < le_device_db_max_count(); i++)
    {
        int db_addr_type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t db_addr;
        le_device_db_info(i, &db_addr_type, db_addr, NULL);
        if (db_addr_type != BD_ADDR_TYPE_UNKNOWN)
        {
            gap_whitelist_add(db_addr_type, db_addr);
            added++;
        }
    }
    if (added > 0)
    {
        if (gap_connect_with_whitelist() == ERROR_CODE_SUCCESS)
        {
            DBG("BLE: Started whitelist connection for %d bonded device(s)\n", added);
        }
        else
        {
            DBG("BLE: Whitelist connect busy, will retry\n");
            ble_scan_restarts_at = make_timeout_time_ms(1000);
        }
    }
}

static inline void ble_restart_reconnection(void)
{
    ble_connecting_handle = HCI_CON_HANDLE_INVALID;
    ble_scan_restarts_at = get_absolute_time();
}

static void ble_hids_client_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// Start HIDS GATT discovery after encryption is established.
// On failure, abandon this connection and try the next device.
static void ble_start_hids_client(hci_con_handle_t con_handle)
{
    uint8_t status = hids_client_connect(con_handle, ble_hids_client_handler,
                                         HID_PROTOCOL_MODE_REPORT, NULL);
    if (status != ERROR_CODE_SUCCESS)
    {
        DBG("BLE: HIDS connect failed: 0x%02x\n", status);
        gap_disconnect(con_handle);
        ble_restart_reconnection();
    }
}

void ble_set_hid_leds(uint8_t leds)
{
    if (ble_hid_leds != leds)
    {
        ble_hid_leds = leds;
        ble_hid_leds_at = get_absolute_time();
    }
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
        uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
        uint16_t cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
        if (status != ERROR_CODE_SUCCESS)
        {
            hci_con_handle_t failed_handle = ble_connecting_handle;
            ble_restart_reconnection();
            if (failed_handle != HCI_CON_HANDLE_INVALID)
                gap_disconnect(failed_handle);
            DBG("BLE: HID service connection failed - Status: 0x%02x, CID: 0x%04x\n", status, cid);
            break;
        }
        ble_restart_reconnection();
        int slot = ble_hids_cid_to_hid_slot(cid);
        const uint8_t *descriptor = hids_client_descriptor_storage_get_descriptor_data(cid, 0);
        uint16_t descriptor_len = hids_client_descriptor_storage_get_descriptor_len(cid, 0);
        if (kbd_mount(slot, descriptor, descriptor_len))
        {
            if (ble_count_kbd < MAX_NR_HIDS_CLIENTS)
                ble_kbd_cids[ble_count_kbd++] = cid;
            ble_hid_leds_at = get_absolute_time();
        }
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
        {
            for (uint8_t i = 0; i < ble_count_kbd; i++)
            {
                if (ble_kbd_cids[i] == cid)
                {
                    ble_kbd_cids[i] = ble_kbd_cids[--ble_count_kbd];
                    break;
                }
            }
        }
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
    if (ble_shutting_down)
        return;

    switch (hci_event_packet_get_type(packet))
    {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING)
        {
            DBG("BLE: Bluetooth LE Central ready and working!\n");
            ble_connect_with_whitelist();
        }
        break;

    case GAP_EVENT_ADVERTISING_REPORT:
    {
        // Only process advertisements during pairing mode
        if (!ble_pairing || ble_scan_restarts_at)
            break;

        bd_addr_t event_addr;
        gap_event_advertising_report_get_address(packet, event_addr);
        uint8_t addr_type = gap_event_advertising_report_get_address_type(packet);

        uint8_t data_length = gap_event_advertising_report_get_data_length(packet);
        const uint8_t *data = gap_event_advertising_report_get_data(packet);

        // Require HID service in advertisement for new devices
        if (!ad_data_contains_uuid16(data_length, data,
                                     ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE))
            break;

        uint8_t connect_status = gap_connect(event_addr, addr_type);
        if (connect_status == ERROR_CODE_SUCCESS)
        {
            DBG("BLE: Found HID %s, connecting...\n", bd_addr_to_str(event_addr));
            gap_stop_scan();
            ble_scan_restarts_at = make_timeout_time_ms(BLE_CONNECT_TIMEOUT_MS);
        }
        else
        {
            DBG("BLE: Found HID %s, connect failed with status 0x%02x\n",
                bd_addr_to_str(event_addr), connect_status);
        }
        break;
    }

    case HCI_EVENT_LE_META:
    {
        uint8_t subevent_code = hci_event_le_meta_get_subevent_code(packet);
        switch (subevent_code)
        {
        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
        {
            uint8_t status = hci_subevent_le_connection_complete_get_status(packet);
            if (status != ERROR_CODE_SUCCESS)
            {
                DBG("BLE: LE Connection failed - Status: 0x%02x\n", status);
                ble_restart_reconnection();
                break;
            }
            hci_con_handle_t con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
            ble_connecting_handle = con_handle;
            ble_scan_restarts_at = make_timeout_time_ms(BLE_CONNECT_TIMEOUT_MS);
            sm_request_pairing(con_handle);
            DBG("BLE: LE Connected 0x%04x, requesting encryption\n", con_handle);
            break;
        }
        }
        break;
    }

    case HCI_EVENT_DISCONNECTION_COMPLETE:
    {
        hci_con_handle_t con_handle = hci_event_disconnection_complete_get_connection_handle(packet);
        DBG("BLE: Disconnection Complete - Handle: 0x%04x\n", con_handle);
        if (ble_connecting_handle == con_handle)
            ble_restart_reconnection();
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
    if (ble_shutting_down)
        return;

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
    {
        hci_con_handle_t handle = sm_event_pairing_complete_get_handle(packet);
        if (sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS)
        {
            DBG("BLE: Pairing complete\n");
            ble_pairing = false;
            led_blink(false);
            if (handle == ble_connecting_handle)
                ble_start_hids_client(handle);
        }
        else
        {
            DBG("BLE: Pairing failed: 0x%02x\n",
                sm_event_pairing_complete_get_reason(packet));
            if (handle == ble_connecting_handle)
                ble_restart_reconnection();
            gap_disconnect(handle);
        }
        break;
    }

    case SM_EVENT_REENCRYPTION_COMPLETE:
    {
        hci_con_handle_t handle = sm_event_reencryption_complete_get_handle(packet);
        uint8_t status = sm_event_reencryption_complete_get_status(packet);
        if (status == ERROR_CODE_SUCCESS)
        {
            DBG("BLE: Re-encryption complete\n");
            if (handle == ble_connecting_handle)
                ble_start_hids_client(handle);
        }
        else
        {
            DBG("BLE: Re-encryption failed: 0x%02x\n", status);
            if (status == ERROR_CODE_PIN_OR_KEY_MISSING)
            {
                DBG("BLE: Deleting bond\n");
                bd_addr_t addr;
                uint8_t addr_type = sm_event_reencryption_complete_get_addr_type(packet);
                sm_event_reencryption_complete_get_address(packet, addr);
                gap_delete_bonding(addr_type, addr);
            }
            if (handle == ble_connecting_handle)
                ble_restart_reconnection();
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
    ble_connecting_handle = HCI_CON_HANDLE_INVALID;
    ble_count_kbd = 0;
    ble_count_mou = 0;
    ble_count_pad = 0;
    ble_hid_leds_at = 0;

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
    if (!ble_initialized || ble_shutting_down)
    {
        if (!ble_shutting_down && cyw_get_rf_enable() && ble_enabled)
        {
            ble_init_stack();
            ble_initialized = true;
            ble_scan_restarts_at = make_timeout_time_ms(100);
        }
        return;
    }

    if (ble_hid_leds_at &&
        absolute_time_diff_us(get_absolute_time(), ble_hid_leds_at) < 0)
    {
        ble_hid_leds_at = 0;
        for (uint8_t i = 0; i < ble_count_kbd; i++)
            if (hids_client_send_write_report(ble_kbd_cids[i], 0,
                                              HID_REPORT_TYPE_OUTPUT, &ble_hid_leds,
                                              sizeof(ble_hid_leds)) ==
                ERROR_CODE_COMMAND_DISALLOWED) // Retry only this error
                ble_hid_leds_at = make_timeout_time_ms(100);
    }

    if (ble_scan_restarts_at &&
        absolute_time_diff_us(get_absolute_time(), ble_scan_restarts_at) < 0)
    {
        ble_scan_restarts_at = 0;
        if (ble_connecting_handle != HCI_CON_HANDLE_INVALID)
        {
            gap_disconnect(ble_connecting_handle);
            ble_connecting_handle = HCI_CON_HANDLE_INVALID;
        }
        gap_connect_cancel();
        if (ble_pairing)
            gap_start_scan();
        else
            ble_connect_with_whitelist();
    }
}

static void ble_set_config(uint8_t ble)
{
    switch (ble)
    {
    case 0:
        ble_shutdown();
        break;
    case 1:
        ble_pairing = false;
        led_blink(false);
        ble_scan_restarts_at = get_absolute_time();
        break;
    case 2:
        if (cyw_get_rf_enable())
        {
            ble_pairing = true;
            led_blink(true);
            ble_scan_restarts_at = get_absolute_time();
        }
        break;
    case 86:
        for (int i = le_device_db_max_count() - 1; i >= 0; i--)
        {
            int db_addr_type = BD_ADDR_TYPE_UNKNOWN;
            bd_addr_t db_addr;
            le_device_db_info(i, &db_addr_type, db_addr, NULL);
            if (db_addr_type != BD_ADDR_TYPE_UNKNOWN)
                gap_delete_bonding(db_addr_type, db_addr);
        }
        ble_shutdown();
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
    ble_connecting_handle = HCI_CON_HANDLE_INVALID;
    ble_scan_restarts_at = 0;
    ble_hid_leds_at = 0;
    if (ble_initialized)
    {
        ble_shutting_down = true;
        gap_stop_scan();
        gap_connect_cancel();
        gap_whitelist_clear();
        // Poll until btstack completes the halting sequence.
        hci_power_control(HCI_POWER_OFF);
        while (hci_get_state() != HCI_STATE_OFF)
            main_task();
        assert(ble_count_kbd == 0);
        assert(ble_count_mou == 0);
        assert(ble_count_pad == 0);
        hci_remove_event_handler(&hci_event_callback_registration);
        sm_remove_event_handler(&sm_event_callback_registration);
        hids_client_deinit();
        sm_deinit();
        l2cap_deinit();
        btstack_memory_deinit();
        btstack_crypto_deinit(); // OMG! This was so hard to find.
        ble_shutting_down = false;
    }
    ble_initialized = false;
}

int ble_status_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    if (ble_enabled)
    {
        if (cyw_get_rf_enable())
            snprintf(buf, buf_size, STR_STATUS_BLE_FULL,
                     ble_count_kbd, ble_count_kbd == 1 ? STR_KEYBOARD_SINGULAR : STR_KEYBOARD_PLURAL,
                     ble_count_mou, ble_count_mou == 1 ? STR_MOUSE_SINGULAR : STR_MOUSE_PLURAL,
                     ble_count_pad, ble_count_pad == 1 ? STR_GAMEPAD_SINGULAR : STR_GAMEPAD_PLURAL,
                     ble_pairing ? STR_BLE_PAIRING : "");
        else
            snprintf(buf, buf_size, STR_STATUS_BLE_SIMPLE, STR_RF_OFF);
    }
    else
    {
        snprintf(buf, buf_size, STR_STATUS_BLE_SIMPLE, STR_DISABLED);
    }
    return -1;
}

void ble_load_enabled(const char *str, size_t len)
{
    str_parse_uint8(&str, &len, &ble_enabled);
    if (ble_enabled > 1)
        ble_enabled = 0;
    ble_set_config(ble_enabled);
}

bool ble_set_enabled(uint8_t ble)
{
    if (ble > 2 && ble != 86)
        return false;
    ble_set_config(ble);
    if (ble == 86)
        ble = 0;
    if (ble > 1)
        ble = 1;
    if (ble_enabled != ble)
    {
        ble_enabled = ble;
        cfg_save();
    }
    return true;
}

uint8_t ble_get_enabled(void)
{
    return ble_enabled;
}

#endif /* RP6502_RIA_W */
