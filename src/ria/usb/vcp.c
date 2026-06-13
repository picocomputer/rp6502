/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "usb/vcp.h"
#include "str/str.h"
#include "sys/cfg.h"
#include "usb/usb.h"
#include <tusb.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_VCP)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// TinyUSB wraps all serial devices as CDC, which is
// technically incorrect for FTDI, CP210X, CH34X, and PL2303.
// VCP (Virtual COM Port) is a better umbrella term.

__in_flash("vcp_string") static const char vcp_string[] = "VCP";
static_assert(sizeof(vcp_string) == 3 + 1);

typedef struct
{
    bool mounted;
    bool opened;
    bool nfc_checked; // hashed against the current NFC device hash
    uint8_t daddr;
} vcp_t;
static vcp_t vcp_mounts[CFG_TUH_CDC];
static_assert(CFG_TUH_CDC <= 10); // one char 0-9 in "VCP0:"

// NFC device tracking: hash identifies a specific USB VCP device.
#define VCP_NFC_HASH_SIZE 128
static char vcp_nfc_device_hash[VCP_NFC_HASH_SIZE];
static int vcp_nfc_device_idx = -1;

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

// Convert USB string descriptor to ASCII for hashing.
static void vcp_desc_string_to_ascii(const void *desc_buf, char *dest, size_t dest_size)
{
    const tusb_desc_string_t *desc = desc_buf;
    uint16_t ulen = usb_desc_string_ulen(desc_buf, USB_DESC_STRING_BUF_SIZE);
    size_t pos = 0;
    for (uint16_t i = 0; i < ulen && pos < dest_size - 1; i++)
    {
        uint16_t ch = desc->utf16le[i];
        dest[pos++] = (ch >= 0x20 && ch <= 0x7E) ? (char)ch : '\x7F';
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
        char vendor[USB_DESC_STRING_MAX_CHAR_LEN + 1];
        char product[USB_DESC_STRING_MAX_CHAR_LEN + 1];
        char comname[sizeof(vcp_string) + 2];
        vendor[0] = '\0';
        product[0] = '\0';
        const void *desc = usb_string_fetch_manufacturer(dev->daddr);
        if (desc)
            usb_desc_string_to_oem(desc, USB_DESC_STRING_BUF_SIZE, vendor, sizeof(vendor));
        desc = usb_string_fetch_product(dev->daddr);
        if (desc)
            usb_desc_string_to_oem(desc, USB_DESC_STRING_BUF_SIZE, product, sizeof(product));
        snprintf(comname, sizeof(comname), "%s%d", vcp_string, state);
        int n = snprintf(buf, buf_size, STR_STATUS_CDC, comname,
                         vendor[0] ? vendor : vcp_alt_vendor_name(vid, pid),
                         product);
        if (state == vcp_nfc_device_idx && n >= 1 && n < (int)buf_size && buf[n - 1] == '\n')
            snprintf(buf + n - 1, buf_size - (n - 1), " (NFC)\n");
    }
    return state + 1;
}

bool vcp_std_handles(const char *name)
{
    if (strncasecmp(name, vcp_string, sizeof(vcp_string) - 1) != 0)
        return false;
    if (!isdigit((unsigned char)name[3]))
        return false;
    if (name[4] != ':')
        return false;
    return true;
}

// Set baudrate, line format, and assert DTR/RTS. Returns true on success.
static bool vcp_configure_and_connect(uint8_t idx, uint32_t baudrate,
                                      uint8_t data_bits, uint8_t parity,
                                      uint8_t stop_bits)
{
    if (!tuh_cdc_set_baudrate(idx, baudrate, NULL, 0))
        return false;
    if (!tuh_cdc_set_data_format(idx, stop_bits, parity, data_bits, NULL, 0))
        return false;
    if (!tuh_cdc_connect(idx, NULL, 0))
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
    uint8_t idx = name[3] - '0';
    if (idx >= CFG_TUH_CDC)
    {
        *err = API_ENODEV;
        return -1;
    }
    if ((int)idx == vcp_nfc_device_idx)
    {
        *err = API_EBUSY;
        return -1;
    }
    if (!vcp_mounts[idx].mounted)
    {
        *err = API_ENODEV;
        return -1;
    }
    if (vcp_mounts[idx].opened)
    {
        *err = API_EBUSY;
        return -1;
    }

    uint32_t baudrate = 115200;
    uint8_t data_bits = 8;
    uint8_t parity = CDC_LINE_CODING_PARITY_NONE;
    uint8_t stop_bits = CDC_LINE_CODING_STOP_BITS_1;
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
            if (data_bits < 5 || data_bits > 8)
            {
                *err = API_EINVAL;
                return -1;
            }
            // Parity
            params++;
            char p = toupper((unsigned char)*params);
            switch (p)
            {
            case 'N':
                parity = CDC_LINE_CODING_PARITY_NONE;
                break;
            case 'O':
                parity = CDC_LINE_CODING_PARITY_ODD;
                break;
            case 'E':
                parity = CDC_LINE_CODING_PARITY_EVEN;
                break;
            case 'M':
                parity = CDC_LINE_CODING_PARITY_MARK;
                break;
            case 'S':
                parity = CDC_LINE_CODING_PARITY_SPACE;
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
                    stop_bits = CDC_LINE_CODING_STOP_BITS_1_5;
                    params += 3;
                }
                else
                {
                    stop_bits = CDC_LINE_CODING_STOP_BITS_1;
                    params++;
                }
            }
            else if (*params == '2')
            {
                stop_bits = CDC_LINE_CODING_STOP_BITS_2;
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

    if (!vcp_configure_and_connect(idx, baudrate, data_bits, parity, stop_bits))
    {
        *err = API_EIO;
        return -1;
    }

    DBG("VCP%d: open %lu,%d%c%s\n", idx, (unsigned long)baudrate, data_bits,
        "NOEMS"[parity], CDC_LINE_CODING_STOP_BITS_TEXT(stop_bits));
    vcp_mounts[idx].opened = true;
    return idx;
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

// Build the device identity hash; false when a string fetch couldn't run.
static bool vcp_hash_dev(uint8_t idx, char *hash)
{
    uint16_t vid, pid;
    tuh_vid_pid_get(vcp_mounts[idx].daddr, &vid, &pid);
    tusb_desc_device_t dev_desc;
    uint16_t bcd = 0;
    if (tuh_descriptor_get_device_local(vcp_mounts[idx].daddr, &dev_desc))
        bcd = dev_desc.bcdDevice;
    int n = snprintf(hash, VCP_NFC_HASH_SIZE, "%04X:%04X:%04X:",
                     vid, pid, bcd);
    if (n < 0 || n >= VCP_NFC_HASH_SIZE)
        return false;
    const void *desc = usb_string_fetch_manufacturer(vcp_mounts[idx].daddr);
    if (!desc)
        return false;
    vcp_desc_string_to_ascii(desc, hash + n, VCP_NFC_HASH_SIZE - n);
    n += strlen(hash + n);
    if (n < VCP_NFC_HASH_SIZE - 1)
        hash[n++] = ':';
    desc = usb_string_fetch_product(vcp_mounts[idx].daddr);
    if (!desc)
        return false;
    vcp_desc_string_to_ascii(desc, hash + n, VCP_NFC_HASH_SIZE - n);
    n += strlen(hash + n);
    if (n < VCP_NFC_HASH_SIZE - 1)
        hash[n++] = ':';
    desc = usb_string_fetch_serial(vcp_mounts[idx].daddr);
    if (!desc)
        return false;
    vcp_desc_string_to_ascii(desc, hash + n, VCP_NFC_HASH_SIZE - n);
    return true;
}

// In the FatFs task tier: hash building blocks on USB string fetches.
void vcp_task(void)
{
    if (!vcp_nfc_device_hash[0] || vcp_nfc_device_idx >= 0)
        return;
    for (uint8_t i = 0; i < CFG_TUH_CDC; i++)
        if (vcp_mounts[i].mounted && !vcp_mounts[i].nfc_checked)
        {
            char hash[VCP_NFC_HASH_SIZE];
            if (!vcp_hash_dev(i, hash))
                return; // fetch refused, retry next pass
            vcp_mounts[i].nfc_checked = true;
            if (strcmp(hash, vcp_nfc_device_hash) == 0)
                vcp_nfc_device_idx = i;
            return; // one device per pass
        }
}

void tuh_cdc_mount_cb(uint8_t idx)
{
    if (idx >= CFG_TUH_CDC)
        return;
    tuh_itf_info_t itf_info;
    if (!tuh_cdc_itf_get_info(idx, &itf_info))
        return;
    uint8_t daddr = itf_info.daddr;
    vcp_t *dev = &vcp_mounts[idx];
    memset(dev, 0, sizeof(*dev));
    uint16_t vid, pid;
    tuh_vid_pid_get(daddr, &vid, &pid);
    dev->daddr = daddr;
    dev->mounted = true;
    DBG("VCP%d: mount %04X:%04X dev_addr=%d\n", idx, vid, pid, daddr);
}

void tuh_cdc_umount_cb(uint8_t idx)
{
    DBG("VCP%d: unmount\n", idx);
    if (idx < CFG_TUH_CDC)
    {
        vcp_mounts[idx].mounted = false;
        vcp_mounts[idx].opened = false;
        if ((int)idx == vcp_nfc_device_idx)
            vcp_nfc_device_idx = -1;
    }
}

void vcp_load_nfc_device_hash(const char *str)
{
    size_t len = strlen(str);
    if (len >= VCP_NFC_HASH_SIZE)
        return;
    memcpy(vcp_nfc_device_hash, str, len);
    vcp_nfc_device_hash[len] = '\0';
    vcp_nfc_device_idx = -1;
    for (uint8_t i = 0; i < CFG_TUH_CDC; i++)
        vcp_mounts[i].nfc_checked = false;
}

bool vcp_set_nfc_device_name(const char *name)
{
    if (!name || !name[0])
    {
        if (vcp_nfc_device_hash[0])
        {
            vcp_nfc_device_hash[0] = '\0';
            cfg_save();
        }
        vcp_nfc_device_idx = -1;
        return true;
    }
    if (!vcp_std_handles(name))
        return false;
    uint8_t idx = name[3] - '0';
    if (idx >= CFG_TUH_CDC || !vcp_mounts[idx].mounted)
        return false;
    char hash[VCP_NFC_HASH_SIZE];
    if (!vcp_hash_dev(idx, hash))
        return false;
    strcpy(vcp_nfc_device_hash, hash);
    vcp_nfc_device_idx = idx;
    cfg_save();
    return true;
}

const char *vcp_get_nfc_device_hash(void)
{
    return vcp_nfc_device_hash;
}

int vcp_nfc_open(void)
{
    if (vcp_nfc_device_idx < 0)
        return -1;
    uint8_t idx = (uint8_t)vcp_nfc_device_idx;
    // Matching is deferred, an app may have opened the port first
    if (vcp_mounts[idx].opened)
        return -1;
    if (!vcp_configure_and_connect(idx, 115200, 8,
                                   CDC_LINE_CODING_PARITY_NONE,
                                   CDC_LINE_CODING_STOP_BITS_1))
        return -1;
    DBG("VCP%d: nfc open\n", idx);
    vcp_mounts[idx].opened = true;
    return idx;
}
