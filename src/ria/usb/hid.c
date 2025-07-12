/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb.h"
#include "usb/hid.h"
#include "usb/kbd.h"
#include "usb/mou.h"
#include "usb/pad.h"
#include "pico/time.h"

#define DEBUG_RIA_USB_HID //////////////

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_HID)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...) \
    {            \
    }
#endif

static uint8_t hid_dev_addr[CFG_TUH_HID];
static bool hid_interrupt_received[CFG_TUH_HID];
static uint8_t hid_report_buffer[CFG_TUH_HID][64];   // Buffer for manual reports
static absolute_time_t hid_mount_time[CFG_TUH_HID];  // Track when each device was mounted
static bool hid_manual_request_pending[CFG_TUH_HID]; // Track if manual request is pending
static bool hid_supports_interrupt[CFG_TUH_HID];     // Track if device has interrupt IN endpoint

#define HID_INTERRUPT_TIMEOUT_MS 1000 // Wait 1 second for interrupt reports before trying manual

// Check if device has interrupt IN endpoint by examining configuration descriptor
static bool hid_check_interrupt_support(uint8_t dev_addr, uint8_t itf_idx)
{
    // Buffer to hold configuration descriptor
    static uint8_t config_desc_buffer[256];

    // Get the configuration descriptor
    uint8_t result = tuh_descriptor_get_configuration_sync(dev_addr, 0, config_desc_buffer, sizeof(config_desc_buffer));
    if (result != XFER_RESULT_SUCCESS)
    {
        DBG("Failed to get configuration descriptor for dev_addr %d\n", dev_addr);
        return false; // Assume no interrupt support if we can't get descriptor
    }

    tusb_desc_configuration_t* config_desc = (tusb_desc_configuration_t*)config_desc_buffer;
    uint8_t* desc_ptr = config_desc_buffer + sizeof(tusb_desc_configuration_t);
    uint8_t* desc_end = config_desc_buffer + config_desc->wTotalLength;

    uint8_t current_itf_idx = 0;
    bool found_target_interface = false;

    // Parse through all descriptors in the configuration
    while (desc_ptr < desc_end)
    {
        uint8_t desc_len = desc_ptr[0];
        uint8_t desc_type = desc_ptr[1];

        if (desc_len < 2) break; // Invalid descriptor

        if (desc_type == TUSB_DESC_INTERFACE)
        {
            tusb_desc_interface_t* itf_desc = (tusb_desc_interface_t*)desc_ptr;
            if (itf_desc->bInterfaceClass == TUSB_CLASS_HID)
            {
                if (current_itf_idx == itf_idx)
                {
                    found_target_interface = true;
                    DBG("Found target HID interface %d at interface number %d\n", itf_idx, itf_desc->bInterfaceNumber);
                }
                current_itf_idx++;
            }
        }
        else if (desc_type == TUSB_DESC_ENDPOINT && found_target_interface)
        {
            tusb_desc_endpoint_t* ep_desc = (tusb_desc_endpoint_t*)desc_ptr;

            // Check if this is an interrupt IN endpoint
            if ((ep_desc->bmAttributes.xfer == TUSB_XFER_INTERRUPT) &&
                (ep_desc->bEndpointAddress & TUSB_DIR_IN_MASK))
            {
                DBG("Found interrupt IN endpoint for dev_addr %d, itf_idx %d: EP 0x%02X\n",
                    dev_addr, itf_idx, ep_desc->bEndpointAddress);
                return true;
            }
        }

        desc_ptr += desc_len;
    }

    DBG("No interrupt IN endpoint found for dev_addr %d, itf_idx %d\n", dev_addr, itf_idx);
    return false;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *report, uint16_t len)
{
    (void)len;

    // Mark that we received an interrupt report for this device
    hid_interrupt_received[idx] = true;

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);
    switch (itf_protocol)
    {
    case HID_ITF_PROTOCOL_KEYBOARD:
        kbd_report(dev_addr, idx, (hid_keyboard_report_t const *)report);
        break;
    case HID_ITF_PROTOCOL_MOUSE:
        mou_report((hid_mouse_report_t const *)report);
        break;
    case HID_ITF_PROTOCOL_NONE:
        pad_report(dev_addr, report, len);
        break;
    }

    // Continue requesting reports
    tuh_hid_receive_report(dev_addr, idx);
}

void hid_print_status(void)
{
    int count_keyboard = 0;
    int count_mouse = 0;
    int count_gamepad = 0;
    int count_unspecified = 0;
    for (int idx = 0; idx < CFG_TUH_HID; idx++)
    {
        uint8_t dev_addr = hid_dev_addr[idx];
        if (dev_addr)
        {
            switch (tuh_hid_interface_protocol(dev_addr, idx))
            {
            case HID_ITF_PROTOCOL_KEYBOARD:
                count_keyboard++;
                break;
            case HID_ITF_PROTOCOL_MOUSE:
                count_mouse++;
                break;
            case HID_ITF_PROTOCOL_NONE:
                // Try to distinguish gamepads if possible, otherwise count as gamepad
                count_gamepad++;
                break;
            default:
                count_unspecified++;
                break;
            }
        }
    }
    printf("USB HID: %d keyboard%s, %d %s, %d gamepad%s",
           count_keyboard, count_keyboard == 1 ? "" : "s",
           count_mouse, count_mouse == 1 ? "mouse" : "mice",
           count_gamepad, count_gamepad == 1 ? "" : "s");
    if (count_unspecified)
        printf(", %d unspecified\n", count_unspecified);
    else
        printf("\n");
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *desc_report, uint16_t desc_len)
{
    hid_dev_addr[idx] = dev_addr;
    hid_interrupt_received[idx] = false;
    hid_mount_time[idx] = get_absolute_time();
    hid_manual_request_pending[idx] = false;

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);

    DBG("HID device mounted: dev_addr=%d, idx=%d, protocol=%d, desc_len=%d\n",
        dev_addr, idx, itf_protocol, desc_len);

    // Check if device supports interrupt endpoints
    hid_supports_interrupt[idx] = hid_check_interrupt_support(dev_addr, idx);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD)
    {
        kbd_hid_leds_dirty();
    }
    else if (itf_protocol == HID_ITF_PROTOCOL_NONE)
    {
        uint16_t vendor_id;
        uint16_t product_id;
        if (tuh_vid_pid_get(dev_addr, &vendor_id, &product_id))
        {
            DBG("HID gamepad: VID=0x%04X, PID=0x%04X\n", vendor_id, product_id);
            pad_parse_descriptor(dev_addr, desc_report, desc_len, vendor_id, product_id);
        }
        else
        {
            DBG("Failed to get VID/PID for dev_addr %d\n", dev_addr);
        }
    }

    // Try interrupt reports first only if device supports them
    if (hid_supports_interrupt[idx])
    {
        DBG("Device supports interrupt reports, trying interrupt first\n");
        tuh_hid_receive_report(dev_addr, idx);
    }
    else
    {
        DBG("Device does not support interrupt reports, using manual requests\n");
        // Skip timeout and go straight to manual requests for devices without interrupt endpoints
        hid_mount_time[idx] = make_timeout_time_ms(0); // Set to immediate timeout
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    hid_dev_addr[idx] = 0;
    hid_interrupt_received[idx] = false;
    hid_manual_request_pending[idx] = false;
    hid_supports_interrupt[idx] = false;

    // Clean up gamepad descriptor if this was a gamepad device
    pad_cleanup_descriptor(dev_addr);
}

void tuh_hid_get_report_complete_cb(uint8_t dev_addr, uint8_t idx, uint8_t report_id, uint8_t report_type, uint16_t len)
{
    (void)report_id;
    (void)report_type;

    hid_manual_request_pending[idx] = false;

    // Process the report data if we have any
    if (len > 0)
    {
        // Use the report buffer that was filled by tuh_hid_get_report
        uint8_t const *report = hid_report_buffer[idx];
        uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);
        switch (itf_protocol)
        {
        case HID_ITF_PROTOCOL_KEYBOARD:
            kbd_report(dev_addr, idx, (hid_keyboard_report_t const *)report);
            break;
        case HID_ITF_PROTOCOL_MOUSE:
            mou_report((hid_mouse_report_t const *)report);
            break;
        case HID_ITF_PROTOCOL_NONE:
            pad_report(dev_addr, report, len);
            break;
        }
    }

    // If we haven't received an interrupt report yet and device supports interrupts, try to switch
    if (!hid_interrupt_received[idx] && hid_supports_interrupt[idx])
    {
        // Try to switch to interrupt-driven reports
        tuh_hid_receive_report(dev_addr, idx);
    }

    // Don't call tuh_hid_get_report here - let hid_task() handle the next request
    // to avoid potential recursion or stack overflow issues
}

// Call this function periodically to check for devices that need manual requests
void hid_task(void)
{
    for (int idx = 0; idx < CFG_TUH_HID; idx++)
    {
        uint8_t dev_addr = hid_dev_addr[idx];
        if (dev_addr && !hid_manual_request_pending[idx])
        {
            // Check if we need to continue manual requests
            bool need_manual_request = false;

            if (!hid_interrupt_received[idx])
            {
                // Check if enough time has passed since mount for initial timeout
                absolute_time_t now = get_absolute_time();
                int64_t elapsed_ms = absolute_time_diff_us(hid_mount_time[idx], now) / 1000;

                // Use shorter timeout for devices without interrupt support, longer for those with
                uint32_t timeout_ms = hid_supports_interrupt[idx] ? HID_INTERRUPT_TIMEOUT_MS : 1000;

                if (elapsed_ms >= timeout_ms)
                {
                    need_manual_request = true;
                    if (hid_supports_interrupt[idx])
                    {
                        DBG("HID device %d:%d timeout, interrupt supported but not working, trying manual request\n", dev_addr, idx);
                    }
                    else
                    {
                        DBG("HID device %d:%d using manual request (no interrupt endpoint)\n", dev_addr, idx);
                    }
                }
            }
            else if (!hid_supports_interrupt[idx])
            {
                // Device doesn't support interrupts but we've processed at least one report
                // Continue with periodic manual requests
                need_manual_request = true;
            }

            if (need_manual_request)
            {
                // Use report ID 0 (no report ID) for most devices, or get actual ID for gamepads
                uint8_t report_id = 0;
                uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);
                if (itf_protocol == HID_ITF_PROTOCOL_NONE)
                {
                    // For gamepads, get the report ID from the parsed descriptor
                    report_id = pad_get_report_id(dev_addr);
                }

                tuh_hid_get_report(dev_addr, idx, report_id, HID_REPORT_TYPE_INPUT, hid_report_buffer[idx], sizeof(hid_report_buffer[idx]));
                hid_manual_request_pending[idx] = true;
            }
        }
    }
}
