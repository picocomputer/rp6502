/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "usb/vcp.h"
#include "api/oem.h"
#include "fatfs/ff.h"
#include "str/str.h"
#include <tusb.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_VCP)
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// TinyUSB wraps all serial devices as CDC, which is
// technically incorrect for FTDI, CP210X, CH34X, and PL2303.
// VCP (Virtual COM Port) is a better umbrella.

__in_flash("vcp_string") const char vcp_string[] = "VCP";
static_assert(sizeof(vcp_string) == 3 + 1);

#define VCP_UTF16_LEN 32
typedef struct
{
    bool mounted;
    bool opened;
    uint8_t daddr;
    uint16_t vendor[VCP_UTF16_LEN];
    uint16_t product[VCP_UTF16_LEN];
} vcp_t;
static vcp_t vcp[CFG_TUH_CDC];

__in_flash("vcp_ftdi_list") static const uint16_t vcp_ftdi_list[][2] = {CFG_TUH_CDC_FTDI_VID_PID_LIST};
__in_flash("vcp_cp210x_list") static const uint16_t vcp_cp210x_list[][2] = {CFG_TUH_CDC_CP210X_VID_PID_LIST};
__in_flash("vcp_ch34x_list") static const uint16_t vcp_ch34x_list[][2] = {CFG_TUH_CDC_CH34X_VID_PID_LIST};
__in_flash("vcp_pl2303_list") static const uint16_t vcp_pl2303_list[][2] = {CFG_TUH_CDC_PL2303_VID_PID_LIST};
__in_flash("vcp_ftdi_name") static const char vcp_ftdi_name[] = "FTDI";
__in_flash("vcp_cp210x_name") static const char vcp_cp210x_name[] = "CP210X";
__in_flash("vcp_ch34x_name") static const char vcp_ch34x_name[] = "CH34X";
__in_flash("vcp_pl2303_name") static const char vcp_pl2303_name[] = "PL2303";
__in_flash("vcp_cdc_acm_name") static const char vcp_cdc_acm_name[] = "CDC ACM";

// Determine fallback vendor name using the same VID/PID lists as TinyUSB.
static const char *vcp_alt_vendor_name(uint16_t vid, uint16_t pid)
{
    for (size_t i = 0; i < TU_ARRAY_SIZE(vcp_ftdi_list); i++)
        if (vcp_ftdi_list[i][0] == vid && vcp_ftdi_list[i][1] == pid)
            return vcp_ftdi_name;
    for (size_t i = 0; i < TU_ARRAY_SIZE(vcp_cp210x_list); i++)
        if (vcp_cp210x_list[i][0] == vid && vcp_cp210x_list[i][1] == pid)
            return vcp_cp210x_name;
    for (size_t i = 0; i < TU_ARRAY_SIZE(vcp_ch34x_list); i++)
        if (vcp_ch34x_list[i][0] == vid && vcp_ch34x_list[i][1] == pid)
            return vcp_ch34x_name;
    for (size_t i = 0; i < TU_ARRAY_SIZE(vcp_pl2303_list); i++)
        if (vcp_pl2303_list[i][0] == vid && vcp_pl2303_list[i][1] == pid)
            return vcp_pl2303_name;
    return vcp_cdc_acm_name;
}

bool vcp_std_handles(const char *name)
{
    if (strncasecmp(name, vcp_string, 3) != 0)
        return false;
    if (!isdigit((unsigned char)name[3]))
        return false;
    if (name[4] != ':')
        return false;
    return true;
}

int vcp_std_open(const char *name, uint8_t flags)
{
    (void)flags;
    if (strncasecmp(name, vcp_string, 3) != 0)
        return -1;
    if (!isdigit((unsigned char)name[3]))
        return -1;
    if (name[4] != ':')
        return -1;
    // TODO above logic is in vcp_std_handles
    uint8_t idx = name[3] - '0';
    if (idx >= CFG_TUH_CDC)
        return -1;
    if (!vcp[idx].mounted || vcp[idx].opened)
        return -1;

    uint32_t baudrate = 115200; // default
    uint8_t data_bits = 8;      // default
    uint8_t parity = 0;         // default: none
    uint8_t stop_bits = 0;      // default: 1 stop bit

    if (name[5] != '\0')
    {
        const char *params = &name[5];

        // Parse baud rate (required if anything follows the colon)
        if (!isdigit((unsigned char)*params))
            return -1;
        baudrate = 0;
        while (isdigit((unsigned char)*params))
        {
            baudrate = baudrate * 10 + (*params - '0');
            params++;
        }

        // Parse optional format (8N1, 7E2, etc.)
        if (*params == ',')
        {
            params++;

            // Data bits (required if comma present)
            if (!isdigit((unsigned char)*params))
                return -1;
            data_bits = *params - '0';
            params++;

            // Parity (required if comma present)
            char p = toupper((unsigned char)*params);
            switch (p)
            {
            case 'N':
                parity = 0;
                break; // none
            case 'O':
                parity = 1;
                break; // odd
            case 'E':
                parity = 2;
                break; // even
            case 'M':
                parity = 3;
                break; // mark
            case 'S':
                parity = 4;
                break; // space
            default:
                return -1;
            }
            params++;

            if (*params == '1')
                stop_bits = 0; // 1 stop bit
            else if (*params == '2')
                stop_bits = 2; // 2 stop bits
            else
                return -1;
            params++;
        }

        // Must be end of string
        if (*params != '\0')
            return -1;
    }

    // Configure baud rate and line format before connecting
    if (!tuh_cdc_set_baudrate(idx, baudrate, NULL, 0))
        return -1;
    if (!tuh_cdc_set_data_format(idx, stop_bits, parity, data_bits, NULL, 0))
        return -1;

    // Connect establishes DTR and RTS for hardware flow control
    if (!tuh_cdc_connect(idx, NULL, 0))
        return -1;
    vcp[idx].opened = true;

    DBG("VCP open VCP%d %lu,%d%c%d\n", idx, (unsigned long)baudrate, data_bits,
        "NOEMS"[parity], stop_bits == 0 ? 1 : stop_bits);
    return idx;
}

bool vcp_std_close(int idx)
{
    if (!vcp[idx].opened)
        return false;
    DBG("VCP close VCP%d\n", idx);
    tuh_cdc_disconnect(idx, NULL, 0);
    vcp[idx].opened = false;
    return true;
}

int vcp_std_read(int idx, char *buf, uint32_t buf_size, uint32_t *bytes_read)
{
    if (!vcp[idx].mounted || !vcp[idx].opened)
        return -1;
    *bytes_read = tuh_cdc_read(idx, buf, buf_size);
    return 0;
}

int vcp_std_write(int idx, const char *buf, uint32_t buf_size, uint32_t *bytes_written)
{
    if (!vcp[idx].mounted || !vcp[idx].opened)
        return -1;
    uint32_t count = tuh_cdc_write(idx, buf, buf_size);
    if (count > 0)
        tuh_cdc_write_flush(idx);
    *bytes_written = count;
    return 0;
}

static void vcp_vendor_string_cb(tuh_xfer_t *xfer)
{
    (void)xfer;
}

static void vcp_product_string_cb(tuh_xfer_t *xfer)
{
    uint8_t idx = (uint8_t)xfer->user_data;
    tuh_descriptor_get_manufacturer_string(vcp[idx].daddr, 0x0409,
                                           vcp[idx].vendor, sizeof(vcp[idx].vendor),
                                           vcp_vendor_string_cb, xfer->user_data);
}

void tuh_cdc_mount_cb(uint8_t idx)
{
    if (idx >= CFG_TUH_CDC)
        return;
    tuh_itf_info_t itf_info;
    tuh_cdc_itf_get_info(idx, &itf_info);
    uint8_t daddr = itf_info.daddr;
    vcp_t *dev = &vcp[idx];
    memset(dev, 0, sizeof(*dev));
    uint16_t vid, pid;
    tuh_vid_pid_get(daddr, &vid, &pid);
    dev->daddr = daddr;
    dev->mounted = true;

    DBG("VCP mounted: VCP%d %04X:%04X dev_addr=%d\n", idx, vid, pid, daddr);

    tuh_descriptor_get_product_string(daddr, 0x0409,
                                      dev->product, sizeof(dev->product),
                                      vcp_product_string_cb, (uintptr_t)idx);
}

void tuh_cdc_umount_cb(uint8_t idx)
{
    DBG("VCP unmounted: VCP%d\n", idx);
    if (idx < CFG_TUH_CDC)
    {
        vcp[idx].mounted = false;
        vcp[idx].opened = false;
    }
}

int vcp_status_count(void)
{
    int count = 0;
    for (uint8_t idx = 0; idx < CFG_TUH_CDC; idx++)
        if (vcp[idx].mounted)
            count++;
    return count;
}

// Convert raw USB string descriptor (with header) to OEM for display.
static void vcp_utf16_to_oem(const uint16_t *raw, char *dest, size_t dest_size)
{
    const tusb_desc_string_t *desc = (const tusb_desc_string_t *)raw;
    uint16_t ulen = (desc->bLength >= 2) ? (desc->bLength - 2) / 2 : 0;
    if (ulen > VCP_UTF16_LEN - 1)
        ulen = VCP_UTF16_LEN - 1;
    uint16_t cp = oem_get_code_page();
    size_t pos = 0;
    for (uint16_t i = 0; i < ulen && pos < dest_size - 1; i++)
    {
        WCHAR ch = ff_uni2oem(desc->utf16le[i], cp);
        if (ch)
            dest[pos++] = (char)ch;
    }
    dest[pos] = '\0';
}

int vcp_status_response(char *buf, size_t buf_size, int state)
{
    if (state < 0 || state >= CFG_TUH_CDC)
        return -1;
    vcp_t *dev = &vcp[state];
    if (dev->mounted)
    {
        uint16_t vid, pid;
        tuh_vid_pid_get(dev->daddr, &vid, &pid);
        const char *driver = vcp_alt_vendor_name(vid, pid);
        char vendor[VCP_UTF16_LEN + 1];
        char product[VCP_UTF16_LEN + 1];
        char comname[8];
        vcp_utf16_to_oem(dev->vendor, vendor, sizeof(vendor));
        vcp_utf16_to_oem(dev->product, product, sizeof(product));
        snprintf(comname, sizeof(comname), "%s%d", vcp_string, state);
        snprintf(buf, buf_size, STR_STATUS_CDC,
                 comname, vendor[0] ? vendor : driver, product);
    }
    return state + 1;
}
