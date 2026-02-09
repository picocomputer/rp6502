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
// VCP (Virtual COM Port) is a better umbrella term.

__in_flash("vcp_string") const char vcp_string[] = "VCP";
static_assert(sizeof(vcp_string) == 3 + 1);

#define VCP_DESC_STRING_BUF_SIZE 64
#define VCP_DESC_STRING_MAX_CHAR_LEN \
    ((VCP_DESC_STRING_BUF_SIZE - sizeof(tusb_desc_string_t)) / sizeof(uint16_t))
typedef struct
{
    bool mounted;
    bool opened;
    uint8_t daddr;
    uint8_t vendor_desc_string[VCP_DESC_STRING_BUF_SIZE];
    uint8_t product_desc_string[VCP_DESC_STRING_BUF_SIZE];
} vcp_t;
static vcp_t vcp_mounts[CFG_TUH_CDC];
static_assert(CFG_TUH_CDC < 11); // one char 0-9 in "VCP0:"

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

// Convert USB string descriptor to OEM for display.
static void vcp_desc_string_to_oem(const tusb_desc_string_t *desc, char *dest, size_t dest_size)
{
    uint16_t ulen = 0;
    if (desc->bDescriptorType == TUSB_DESC_STRING && desc->bLength >= 2)
    {
        ulen = (desc->bLength - 2) / 2;
        if (ulen > VCP_DESC_STRING_MAX_CHAR_LEN)
            ulen = VCP_DESC_STRING_MAX_CHAR_LEN;
    }
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

int vcp_status_count(void)
{
    int count = 0;
    for (uint8_t idx = 0; idx < CFG_TUH_CDC; idx++)
        if (vcp_mounts[idx].mounted)
            count++;
    return count;
}

int vcp_status_response(char *buf, size_t buf_size, int state)
{
    if (state < 0 || state >= CFG_TUH_CDC)
        return -1;
    vcp_t *dev = &vcp_mounts[state];
    if (dev->mounted)
    {
        uint16_t vid, pid;
        tuh_vid_pid_get(dev->daddr, &vid, &pid);
        const char *driver = vcp_alt_vendor_name(vid, pid);
        char vendor[VCP_DESC_STRING_MAX_CHAR_LEN + 1];
        char product[VCP_DESC_STRING_MAX_CHAR_LEN + 1];
        char comname[sizeof(vcp_string) + 2];
        vcp_desc_string_to_oem(
            (const tusb_desc_string_t *)dev->vendor_desc_string,
            vendor, sizeof(vendor));
        vcp_desc_string_to_oem(
            (const tusb_desc_string_t *)dev->product_desc_string,
            product, sizeof(product));
        snprintf(comname, sizeof(comname), "%s%d", vcp_string, state);
        snprintf(buf, buf_size, STR_STATUS_CDC,
                 comname, vendor[0] ? vendor : driver, product);
    }
    return state + 1;
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

int vcp_std_open(const char *name, uint8_t flags, api_errno *err)
{
    (void)flags;
    if (!vcp_std_handles(name))
    {
        *err = API_ENOENT;
        return -1;
    }
    uint8_t desc = name[3] - '0';
    if (desc >= CFG_TUH_CDC)
    {
        *err = API_ENODEV;
        return -1;
    }
    if (!vcp_mounts[desc].mounted || vcp_mounts[desc].opened)
    {
        *err = vcp_mounts[desc].opened ? API_EBUSY : API_ENODEV;
        return -1;
    }

    uint32_t baudrate = 115200;
    uint8_t data_bits = 8;
    uint8_t parity = 0;
    uint8_t stop_bits = 0;
    if (name[5] != '\0')
    {
        const char *params = &name[5];
        // Parse baud rate
        if (!isdigit((unsigned char)*params))
        {
            *err = API_EINVAL;
            return -1;
        }
        baudrate = 0;
        while (isdigit((unsigned char)*params))
        {
            baudrate = baudrate * 10 + (*params - '0');
            params++;
        }
        if (*params == ',')
        {
            // Parse format (8N1, 7E2, etc.)
            // Data bits
            params++;
            if (!isdigit((unsigned char)*params))
            {
                *err = API_EINVAL;
                return -1;
            }
            data_bits = *params - '0';
            // Parity
            params++;
            char p = toupper((unsigned char)*params);
            switch (p)
            {
            case 'N':
                parity = 0;
                break;
            case 'O':
                parity = 1;
                break;
            case 'E':
                parity = 2;
                break;
            case 'M':
                parity = 3;
                break;
            case 'S':
                parity = 4;
                break;
            default:
                *err = API_EINVAL;
                return -1;
            }
            // Stop bits
            params++;
            if (*params == '1')
            {
                if (params[1] == '.' && params[2] == '5')
                {
                    stop_bits = 1; // 1.5 stop bits
                    params += 3;
                }
                else
                {
                    stop_bits = 0; // 1 stop bit
                    params++;
                }
            }
            else if (*params == '2')
            {
                stop_bits = 2; // 2 stop bits
                params++;
            }
            else
            {
                *err = API_EINVAL;
                return -1;
            }
        }

        // Must be end of string
        if (*params != '\0')
        {
            *err = API_EINVAL;
            return -1;
        }
    }

    // Configure baud rate and line format before connecting
    if (!tuh_cdc_set_baudrate(desc, baudrate, NULL, 0))
    {
        *err = API_EIO;
        return -1;
    }
    if (!tuh_cdc_set_data_format(desc, stop_bits, parity, data_bits, NULL, 0))
    {
        *err = API_EIO;
        return -1;
    }

    // Connect establishes DTR and RTS for hardware flow control
    if (!tuh_cdc_connect(desc, NULL, 0))
    {
        *err = API_EIO;
        return -1;
    }

    DBG("VCP%d: open %lu,%d%c%s\n", desc, (unsigned long)baudrate, data_bits,
        "NOEMS"[parity], stop_bits == 0 ? "1" : stop_bits == 1 ? "1.5"
                                                               : "2");
    vcp_mounts[desc].opened = true;
    return desc;
}

int vcp_std_close(int desc, api_errno *err)
{
    if (!vcp_mounts[desc].opened)
    {
        *err = API_EBADF;
        return -1;
    }
    DBG("VCP%d: close\n", desc);
    tuh_cdc_disconnect(desc, NULL, 0);
    vcp_mounts[desc].opened = false;
    return 0;
}

std_rw_result vcp_std_read(int desc, char *buf, uint32_t buf_size,
                           uint32_t *bytes_read, api_errno *err)
{
    if (!vcp_mounts[desc].mounted || !vcp_mounts[desc].opened)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    *bytes_read = tuh_cdc_read(desc, buf, buf_size);
    return STD_OK;
}

std_rw_result vcp_std_write(int desc, const char *buf, uint32_t buf_size,
                            uint32_t *bytes_written, api_errno *err)
{
    if (!vcp_mounts[desc].mounted || !vcp_mounts[desc].opened)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    uint32_t count = tuh_cdc_write(desc, buf, buf_size);
    if (count > 0)
        tuh_cdc_write_flush(desc);
    *bytes_written = count;
    return STD_OK;
}

static void vcp_vendor_string_cb(tuh_xfer_t *xfer)
{
    (void)xfer;
}

static void vcp_product_string_cb(tuh_xfer_t *xfer)
{
    uint8_t idx = (uint8_t)xfer->user_data;
    tuh_descriptor_get_manufacturer_string(vcp_mounts[idx].daddr, 0x0409,
                                           vcp_mounts[idx].vendor_desc_string,
                                           sizeof(vcp_mounts[idx].vendor_desc_string),
                                           vcp_vendor_string_cb, xfer->user_data);
}

void tuh_cdc_mount_cb(uint8_t idx)
{
    if (idx >= CFG_TUH_CDC)
        return;
    tuh_itf_info_t itf_info;
    tuh_cdc_itf_get_info(idx, &itf_info);
    uint8_t daddr = itf_info.daddr;
    vcp_t *dev = &vcp_mounts[idx];
    memset(dev, 0, sizeof(*dev));
    uint16_t vid, pid;
    tuh_vid_pid_get(daddr, &vid, &pid);
    dev->daddr = daddr;
    dev->mounted = true;

    DBG("VCP%d: mount %04X:%04X dev_addr=%d\n", idx, vid, pid, daddr);

    tuh_descriptor_get_product_string(daddr, 0x0409,
                                      dev->product_desc_string,
                                      sizeof(dev->product_desc_string),
                                      vcp_product_string_cb, (uintptr_t)idx);
}

void tuh_cdc_umount_cb(uint8_t idx)
{
    DBG("VCP%d: unmount\n", idx);
    if (idx < CFG_TUH_CDC)
    {
        vcp_mounts[idx].mounted = false;
        vcp_mounts[idx].opened = false;
    }
}
