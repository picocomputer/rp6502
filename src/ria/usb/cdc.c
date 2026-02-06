/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "usb/cdc.h"
#include "api/oem.h"
#include "fatfs/ff.h"
#include "str/str.h"
#include <tusb.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define DEBUG_RIA_USB_CDC

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_CDC)
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define CDC_UTF16_LEN 32

typedef struct
{
    bool mounted;
    bool opened;
    uint8_t daddr;
    uint16_t vendor[CDC_UTF16_LEN];
    uint16_t product[CDC_UTF16_LEN];
} cdc_t;

static cdc_t cdc[CFG_TUH_CDC];

void cdc_init(void)
{
    memset(cdc, 0, sizeof(cdc));
}

void cdc_task(void)
{
    for (uint8_t idx = 0; idx < CFG_TUH_CDC; idx++)
    {
        if (cdc[idx].mounted)
            tuh_cdc_write_flush(idx);
    }
}

int cdc_count(void)
{
    int count = 0;
    for (uint8_t idx = 0; idx < CFG_TUH_CDC; idx++)
        if (cdc[idx].mounted)
            count++;
    return count;
}

// Determine driver type using the same VID/PID lists as TinyUSB.
static const char *cdc_alt_vendor_name(uint16_t vid, uint16_t pid)
{
    static const uint16_t ftdi_list[][2] = {CFG_TUH_CDC_FTDI_VID_PID_LIST};
    for (size_t i = 0; i < TU_ARRAY_SIZE(ftdi_list); i++)
        if (ftdi_list[i][0] == vid && ftdi_list[i][1] == pid)
            return "FTDI";
    static const uint16_t cp210x_list[][2] = {CFG_TUH_CDC_CP210X_VID_PID_LIST};
    for (size_t i = 0; i < TU_ARRAY_SIZE(cp210x_list); i++)
        if (cp210x_list[i][0] == vid && cp210x_list[i][1] == pid)
            return "CP210X";
    static const uint16_t ch34x_list[][2] = {CFG_TUH_CDC_CH34X_VID_PID_LIST};
    for (size_t i = 0; i < TU_ARRAY_SIZE(ch34x_list); i++)
        if (ch34x_list[i][0] == vid && ch34x_list[i][1] == pid)
            return "CH34X";
    static const uint16_t pl2303_list[][2] = {CFG_TUH_CDC_PL2303_VID_PID_LIST};
    for (size_t i = 0; i < TU_ARRAY_SIZE(pl2303_list); i++)
        if (pl2303_list[i][0] == vid && pl2303_list[i][1] == pid)
            return "PL2303";
    return "ACM";
}

// Convert raw USB string descriptor (with header) to OEM for display.
static void cdc_utf16_to_oem(const uint16_t *raw, char *dest, size_t dest_size)
{
    const tusb_desc_string_t *desc = (const tusb_desc_string_t *)raw;
    uint16_t ulen = (desc->bLength >= 2) ? (desc->bLength - 2) / 2 : 0;
    if (ulen > CDC_UTF16_LEN - 1)
        ulen = CDC_UTF16_LEN - 1;
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

int cdc_status_response(char *buf, size_t buf_size, int state)
{
    if (state < 0 || state >= CFG_TUH_CDC)
        return -1;
    cdc_t *dev = &cdc[state];
    if (dev->mounted)
    {
        uint16_t vid, pid;
        tuh_vid_pid_get(dev->daddr, &vid, &pid);
        const char *driver = cdc_alt_vendor_name(vid, pid);
        char vendor[CDC_UTF16_LEN + 1];
        char product[CDC_UTF16_LEN + 1];
        char comname[8];
        cdc_utf16_to_oem(dev->vendor, vendor, sizeof(vendor));
        cdc_utf16_to_oem(dev->product, product, sizeof(product));
        snprintf(comname, sizeof(comname), "COM%d", state);
        snprintf(buf, buf_size, STR_STATUS_CDC,
                 comname, vendor[0] ? vendor : driver, product);
    }
    return state + 1;
}

int cdc_open(const char *name)
{
    if (strncasecmp(name, "COM", 3) != 0)
        return -1;
    if (!isdigit((unsigned char)name[3]))
        return -1;
    uint8_t idx = name[3] - '0';
    if (name[4] != ':' && name[4] != '\0')
        return -1;
    if (idx >= CFG_TUH_CDC)
        return -1;
    if (!cdc[idx].mounted || cdc[idx].opened)
        return -1;
    // Connect establishes DTR and RTS for hardware flow control
    if (!tuh_cdc_connect(idx, NULL, 0))
        return -1;
    cdc[idx].opened = true;
    DBG("CDC open COM%d\n", idx);
    return idx;
}

bool cdc_close(int idx)
{
    if (idx < 0 || idx >= CFG_TUH_CDC)
        return false;
    if (!cdc[idx].opened)
        return false;
    DBG("CDC close COM%d\n", idx);
    // Disconnect clears DTR and RTS
    tuh_cdc_disconnect(idx, NULL, 0);
    cdc[idx].opened = false;
    return true;
}

int cdc_rx(int idx, char *buf, int buf_size)
{
    if (idx < 0 || idx >= CFG_TUH_CDC)
        return -1;
    if (!cdc[idx].mounted || !cdc[idx].opened)
        return -1;
    return (int)tuh_cdc_read(idx, buf, (uint32_t)buf_size);
}

int cdc_tx(int idx, const char *buf, int buf_size)
{
    if (idx < 0 || idx >= CFG_TUH_CDC)
        return -1;
    if (!cdc[idx].mounted || !cdc[idx].opened)
        return -1;
    uint32_t count = tuh_cdc_write(idx, buf, (uint32_t)buf_size);
    tuh_cdc_write_flush(idx);
    return (int)count;
}

// Line coding and modem control

bool cdc_set_baudrate(int idx, uint32_t baudrate)
{
    if (idx < 0 || idx >= CFG_TUH_CDC)
        return false;
    if (!cdc[idx].mounted || !cdc[idx].opened)
        return false;
    DBG("CDC set baud=%lu COM%d\n", (unsigned long)baudrate, idx);
    return tuh_cdc_set_baudrate(idx, baudrate, NULL, 0);
}

bool cdc_set_data_format(int idx, uint8_t stop_bits, uint8_t parity, uint8_t data_bits)
{
    if (idx < 0 || idx >= CFG_TUH_CDC)
        return false;
    if (!cdc[idx].mounted || !cdc[idx].opened)
        return false;
    DBG("CDC set format stop=%d par=%d data=%d COM%d\n", stop_bits, parity, data_bits, idx);
    return tuh_cdc_set_data_format(idx, stop_bits, parity, data_bits, NULL, 0);
}

bool cdc_set_dtr(int idx, bool state)
{
    if (idx < 0 || idx >= CFG_TUH_CDC)
        return false;
    if (!cdc[idx].mounted || !cdc[idx].opened)
        return false;
    DBG("CDC set DTR=%d COM%d\n", state, idx);
    return tuh_cdc_set_dtr(idx, state, NULL, 0);
}

bool cdc_set_rts(int idx, bool state)
{
    if (idx < 0 || idx >= CFG_TUH_CDC)
        return false;
    if (!cdc[idx].mounted || !cdc[idx].opened)
        return false;
    DBG("CDC set RTS=%d COM%d\n", state, idx);
    return tuh_cdc_set_rts(idx, state, NULL, 0);
}

static void cdc_vendor_string_cb(tuh_xfer_t *xfer)
{
    uint8_t idx = (uint8_t)(uintptr_t)xfer->user_data;
    if (idx < CFG_TUH_CDC && cdc[idx].mounted &&
        xfer->result == XFER_RESULT_SUCCESS)
    {
        DBG("CDC COM%d vendor ok\n", idx);
    }
}

static void cdc_product_string_cb(tuh_xfer_t *xfer)
{
    uint8_t idx = (uint8_t)(uintptr_t)xfer->user_data;
    if (idx < CFG_TUH_CDC && cdc[idx].mounted &&
        xfer->result == XFER_RESULT_SUCCESS)
    {
        DBG("CDC COM%d product ok\n", idx);
    }
    // Chain: fetch vendor string next, writing directly into vendor[]
    if (idx < CFG_TUH_CDC && cdc[idx].mounted)
    {
        tuh_descriptor_get_manufacturer_string(cdc[idx].daddr, 0x0409,
                                               cdc[idx].vendor, sizeof(cdc[idx].vendor),
                                               cdc_vendor_string_cb, (uintptr_t)idx);
    }
}

void tuh_cdc_mount_cb(uint8_t idx)
{
    if (idx >= CFG_TUH_CDC)
        return;

    tuh_itf_info_t itf_info;
    tuh_cdc_itf_get_info(idx, &itf_info);
    uint8_t daddr = itf_info.daddr;

    cdc_t *dev = &cdc[idx];
    memset(dev, 0, sizeof(*dev));
    uint16_t vid, pid;
    tuh_vid_pid_get(daddr, &vid, &pid);
    dev->daddr = daddr;
    dev->mounted = true;

    DBG("CDC mounted: COM%d %s %04X:%04X dev_addr=%d\n",
        idx, cdc_alt_vendor_name(vid, pid), vid, pid, daddr);

    // Start async product string fetch (chains to vendor)
    tuh_descriptor_get_product_string(daddr, 0x0409,
                                      dev->product, sizeof(dev->product),
                                      cdc_product_string_cb, (uintptr_t)idx);
}

void tuh_cdc_umount_cb(uint8_t idx)
{
    DBG("CDC unmounted: COM%d\n", idx);
    if (idx < CFG_TUH_CDC)
    {
        cdc[idx].mounted = false;
        cdc[idx].opened = false;
    }
}
