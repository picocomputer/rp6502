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

// BTStack includes - minimal set for Classic HID only
#include "btstack.h"
#include "classic/hid_host.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "l2cap.h"
#include "bluetooth_data_types.h"

// We can use the same indexing as hid and xin so long as we keep clear
static uint8_t btx_slot_to_pad_idx(int slot)
{
    return CFG_TUH_HID + PAD_MAX_PLAYERS + slot;
}

// Connection tracking for Classic HID only
typedef struct
{
    bool active;
    uint16_t classic_cid; // Classic HID connection ID
    bd_addr_t remote_addr;
} btx_connection_t;

static btx_connection_t btx_connections[PAD_MAX_PLAYERS];
static bool btx_initialized = false;
static bool btx_pairing_mode = false;
static absolute_time_t btx_pairing_timeout;

// BTStack state - Classic HID only
static btstack_packet_callback_registration_t hci_event_callback_registration;

// Storage for HID descriptors - Classic only
static uint8_t hid_descriptor_storage[100]; // Classic HID descriptor storage (minimal)

// Forward declarations
static void btx_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void btx_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET)
        return;

    uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type)
    {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING)
        {
            DBG("BTX: Bluetooth stack ready\n");
            printf("BTX: Bluetooth stack ready\n");

            if (btx_pairing_mode)
            {
                DBG("BTX: Making device discoverable for pairing\n");
                printf("BTX: Device is now discoverable for pairing\n");
                gap_discoverable_control(1);
                gap_connectable_control(1);
                // Ensure scan modes are enabled
                hci_send_cmd(&hci_write_scan_enable, 0x03);
            }
        }
        else
        {
            DBG("BTX: Bluetooth stack state: %d\n", btstack_event_state_get_state(packet));
            printf("BTX: Bluetooth stack state: %d\n", btstack_event_state_get_state(packet));
        }
        break;

    case HCI_EVENT_PIN_CODE_REQUEST:
    {
        bd_addr_t event_addr;
        hci_event_pin_code_request_get_bd_addr(packet, event_addr);
        DBG("BTX: PIN code request, responding with '0000'\n");
        gap_pin_code_response(event_addr, "0000");
    }
    break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
    {
        bd_addr_t event_addr;
        hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
        uint32_t numeric_value = hci_event_user_confirmation_request_get_numeric_value(packet);
        DBG("BTX: SSP User Confirmation Request: %lu - Auto accepting\n", (unsigned long)numeric_value);
        gap_ssp_confirmation_response(event_addr);
    }
    break;

    case HCI_EVENT_INQUIRY_RESULT:
    {
        DBG("BTX: Inquiry result received\n");
        printf("BTX: Device being discovered by remote device\n");
        // We're not actively doing inquiry, but log if we receive this
    }
    break;

    case HCI_EVENT_INQUIRY_COMPLETE:
    {
        DBG("BTX: Inquiry complete\n");
        printf("BTX: Inquiry process completed\n");
    }
    break;

    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
    {
        bd_addr_t event_addr;
        hci_event_remote_name_request_complete_get_bd_addr(packet, event_addr);
        uint8_t status = hci_event_remote_name_request_complete_get_status(packet);
        if (status == 0)
        {
            const char *name = hci_event_remote_name_request_complete_get_remote_name(packet);
            DBG("BTX: Remote name request complete: %s\n", name);
        }
    }
    break;

    case HCI_EVENT_CONNECTION_REQUEST:
    {
        bd_addr_t event_addr;
        hci_event_connection_request_get_bd_addr(packet, event_addr);
        uint32_t cod = hci_event_connection_request_get_class_of_device(packet);
        DBG("BTX: Connection request from device, CoD: 0x%06lx\n", (unsigned long)cod);
        printf("BTX: Connection request from device, CoD: 0x%06lx\n", (unsigned long)cod);

        // Accept connection requests when in pairing mode OR if we're already paired
        if (btx_pairing_mode)
        {
            DBG("BTX: Accepting connection request (pairing mode)\n");
            printf("BTX: Accepting connection request (pairing mode)\n");
            hci_send_cmd(&hci_accept_connection_request, event_addr, HCI_ROLE_SLAVE);
        }
        else
        {
            // Check if this device is already paired (TODO: implement proper pairing database)
            DBG("BTX: Accepting connection request (assuming known device)\n");
            printf("BTX: Accepting connection request (assuming known device)\n");
            hci_send_cmd(&hci_accept_connection_request, event_addr, HCI_ROLE_SLAVE);
        }
    }
    break;

    case HCI_EVENT_CONNECTION_COMPLETE:
    {
        uint8_t status = hci_event_connection_complete_get_status(packet);
        if (status == ERROR_CODE_SUCCESS)
        {
            bd_addr_t event_addr;
            hci_event_connection_complete_get_bd_addr(packet, event_addr);
            uint16_t handle = hci_event_connection_complete_get_connection_handle(packet);
            DBG("BTX: ACL connection established, handle: 0x%04x\n", handle);
            printf("BTX: ACL connection established, handle: 0x%04x\n", handle);
        }
        else
        {
            DBG("BTX: ACL connection failed, status: 0x%02x\n", status);
            printf("BTX: ACL connection failed, status: 0x%02x\n", status);
        }
    }
    break;

    case HCI_EVENT_COMMAND_COMPLETE:
    {
        uint16_t opcode = little_endian_read_16(packet, 3);
        if (opcode == HCI_OPCODE_HCI_WRITE_SCAN_ENABLE)
        {
            uint8_t status = packet[5];
            if (status == 0)
            {
                printf("BTX: Scan mode successfully enabled\n");
            }
            else
            {
                printf("BTX: Failed to enable scan mode, status: 0x%02x\n", status);
            }
        }
        else if (opcode == HCI_OPCODE_HCI_WRITE_INQUIRY_SCAN_ACTIVITY)
        {
            uint8_t status = packet[5];
            printf("BTX: Inquiry scan activity set, status: 0x%02x\n", status);
        }
        else if (opcode == HCI_OPCODE_HCI_WRITE_PAGE_SCAN_ACTIVITY)
        {
            uint8_t status = packet[5];
            printf("BTX: Page scan activity set, status: 0x%02x\n", status);
        }
    }
    break;

    case HCI_EVENT_HID_META:
    {
        uint8_t subevent = hci_event_hid_meta_get_subevent_code(packet);
        switch (subevent)
        {
        case HID_SUBEVENT_INCOMING_CONNECTION:
        {
            uint16_t cid = hid_subevent_incoming_connection_get_hid_cid(packet);
            DBG("BTX: Incoming HID connection, CID: %u\n", cid);
            printf("BTX: Incoming HID connection, CID: %u\n", cid);
            hid_host_accept_connection(cid, HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT);
        }
        break;

        case HID_SUBEVENT_CONNECTION_OPENED:
        {
            uint8_t status = hid_subevent_connection_opened_get_status(packet);
            if (status == ERROR_CODE_SUCCESS)
            {
                uint16_t cid = hid_subevent_connection_opened_get_hid_cid(packet);
                bd_addr_t addr;
                hid_subevent_connection_opened_get_bd_addr(packet, addr);

                // Find an empty slot
                for (int i = 0; i < PAD_MAX_PLAYERS; i++)
                {
                    if (!btx_connections[i].active)
                    {
                        btx_connections[i].active = true;
                        btx_connections[i].classic_cid = cid;
                        memcpy(btx_connections[i].remote_addr, addr, BD_ADDR_LEN);

                        // Mount the gamepad with no descriptor for now (Bluetooth HID)
                        // TODO: Retrieve and parse HID descriptor for better compatibility
                        bool mounted = pad_mount(btx_slot_to_pad_idx(i), NULL, 0, 0, 0, 0);
                        if (!mounted)
                        {
                            DBG("BTX: Failed to mount gamepad at slot %d\n", i);
                            btx_connections[i].active = false;
                            break;
                        }

                        DBG("BTX: HID gamepad connected at slot %d, CID: %u, pad_idx: %d\n",
                            i, cid, btx_slot_to_pad_idx(i));

                        // Exit pairing mode after successful connection
                        if (btx_pairing_mode)
                        {
                            btx_pairing_mode = false;
                            gap_discoverable_control(0);
                            gap_connectable_control(0);
                            DBG("BTX: Pairing mode disabled after connection\n");
                        }
                        break;
                    }
                }
            }
            else
            {
                DBG("BTX: HID connection failed, status: 0x%02x\n", status);
            }
        }
        break;

        case HID_SUBEVENT_REPORT:
        {
            uint16_t cid = hid_subevent_report_get_hid_cid(packet);
            const uint8_t *report = hid_subevent_report_get_report(packet);
            uint16_t report_len = hid_subevent_report_get_report_len(packet);

            // Find the connection for this CID
            for (int i = 0; i < PAD_MAX_PLAYERS; i++)
            {
                if (btx_connections[i].active && btx_connections[i].classic_cid == cid)
                {
                    // Process HID report and send to gamepad system
                    pad_report(btx_slot_to_pad_idx(i), report, report_len);
                    break;
                }
            }
        }
        break;

        case HID_SUBEVENT_CONNECTION_CLOSED:
        {
            uint16_t cid = hid_subevent_connection_closed_get_hid_cid(packet);

            // Find and clean up the connection
            for (int i = 0; i < PAD_MAX_PLAYERS; i++)
            {
                if (btx_connections[i].active && btx_connections[i].classic_cid == cid)
                {
                    pad_umount(btx_slot_to_pad_idx(i));
                    btx_connections[i].active = false;
                    DBG("BTX: HID gamepad disconnected from slot %d\n", i);
                    break;
                }
            }
        }
        break;
        }
    }
    break;

    default:
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

    // Initialize L2CAP (required for HID Host)
    l2cap_init();

    // Initialize SDP Server
    sdp_init();

    // Add HID Host service record to make us visible to HID devices
    // This advertises that we accept HID connections
    uint8_t hid_service_buffer[250];
    uint8_t *hid_service = hid_service_buffer;
    de_create_sequence(hid_service);
    de_add_number(hid_service, DE_UINT, DE_SIZE_16, BLUETOOTH_SERVICE_CLASS_HUMAN_INTERFACE_DEVICE_SERVICE);
    sdp_register_service(hid_service_buffer);

    // Initialize HID Host for Classic
    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
    hid_host_register_packet_handler(btx_packet_handler);

    // Register for HCI events - must be done before hci_power_control
    hci_event_callback_registration.callback = &btx_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // Configure GAP - act as a receiver/host for HID devices
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(0x01); // Allow role switching (0x01 = switch allowed)
    /* gap_set_class_of_device(0x2540); // Computer/Desktop with HID capability - what gamepads expect */
    gap_set_class_of_device(0x2508); // Toy/Game with HID capability - try more specific gaming device
    gap_set_local_name("RP6502-RIA-W");

    // Enable SSP with automatic accept
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);

    // Set page scan parameters for better discoverability
    gap_set_page_scan_activity(0x0800, 0x0012); // More frequent page scans

    btx_initialized = true;
    DBG("BTX: Bluetooth Classic HID gamepad infrastructure initialized\n");

    // Start the Bluetooth stack
    hci_power_control(HCI_POWER_ON);
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

    // Check for pairing mode timeout (60 seconds)
    if (btx_pairing_mode && absolute_time_diff_us(btx_pairing_timeout, get_absolute_time()) > 0)
    {
        btx_pairing_mode = false;
        gap_discoverable_control(0);
        gap_connectable_control(0);
        DBG("BTX: Pairing mode timed out\n");
        printf("BTX: Pairing mode timed out after 60 seconds\n");
    }

    // For Pico SDK with CYW43, the BTStack run loop is handled automatically
    // We don't need to call btstack_run_loop_execute() here as it would block
    // The Pico SDK integration handles the BTStack event processing
}

bool btx_start_pairing(void)
{
    if (!cyw_ready())
    {
        printf("BTX: Bluetooth radio not ready\n");
        return false;
    }

    if (!btx_initialized)
    {
        return false;
    }

    if (btx_pairing_mode)
    {
        // Already in pairing mode, turn it off
        btx_pairing_mode = false;
        gap_discoverable_control(0);
        gap_connectable_control(0);
        printf("BTX: Pairing mode disabled\n");
    }
    else
    {
        // Start pairing mode with 60 second timeout
        btx_pairing_mode = true;
        btx_pairing_timeout = make_timeout_time_ms(60000); // 60 seconds

        // Explicitly set discoverable and connectable
        printf("BTX: Setting device discoverable and connectable...\n");
        gap_discoverable_control(1);
        gap_connectable_control(1);

        // Set inquiry scan parameters for better discoverability
        hci_send_cmd(&hci_write_inquiry_scan_activity, 0x0800, 0x0012); // Faster scanning
        hci_send_cmd(&hci_write_page_scan_activity, 0x0800, 0x0012);    // Faster paging

        // Also try to set the inquiry scan and page scan manually
        hci_send_cmd(&hci_write_scan_enable, 0x03); // Both inquiry and page scan enabled

        printf("BTX: Pairing mode enabled for 60 seconds - put your gamepad in pairing mode\n");
        printf("BTX: Device discoverable as 'RP6502-RIA-W'\n");
        printf("BTX: For PS4/PS5: Hold Share + PS buttons until light bar flashes\n");
        printf("BTX: For Xbox: Hold Xbox button + Pair button until Xbox button flashes\n");
        printf("BTX: For Switch Pro: Hold Sync button until lights flash\n");
    }
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
            DBG("BTX: Disconnected Bluetooth gamepad at slot %d\n", i);
        }
    }

    DBG("BTX: All Bluetooth gamepad connections disconnected\n");
}

void btx_print_status(void)
{
    if (!cyw_ready())
    {
        printf("BTX: Bluetooth radio not ready\n");
        return;
    }

    if (!btx_initialized)
    {
        printf("BTX: Bluetooth gamepad support not initialized\n");
        return;
    }

    printf("BTX: Bluetooth Classic HID gamepad support active\n");
    printf("BTX: Pairing mode: %s\n", btx_pairing_mode ? "ON" : "OFF");

    int active_count = 0;
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
    {
        if (btx_connections[i].active)
        {
            active_count++;
            printf("  Slot %d: pad_idx=%d, CID=0x%04x (active)\n", i, btx_slot_to_pad_idx(i), btx_connections[i].classic_cid);
        }
    }

    if (active_count == 0)
    {
        printf("  No active Bluetooth gamepad connections\n");
    }
    else
    {
        printf("  %d active Bluetooth gamepad connection%s\n", active_count, active_count == 1 ? "" : "s");
    }

    if (btx_pairing_mode)
    {
        printf("  Device discoverable as 'RP6502-RIA-W'\n");
        printf("  Put your gamepad in pairing mode to connect\n");
    }
    else
    {
        printf("  Use 'set bt' command to start pairing mode\n");
    }
}

void btx_reset(void)
{
    // TODO when called will interrupt pairing session
}

#endif /* RP6502_RIA_W */
