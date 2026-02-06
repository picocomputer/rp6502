/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "usb/cdc.h"
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

// Descriptor slots that map an open() handle to a TinyUSB CDC index.
// A slot is free when tuh_idx == CDC_IDX_INVALID.
#define CDC_MAX_DESC 8
#define CDC_IDX_INVALID 0xFF

typedef struct
{
    uint8_t tuh_idx; // TinyUSB CDC interface index
} cdc_desc_t;

static cdc_desc_t cdc_desc[CDC_MAX_DESC];

// Track which TinyUSB CDC indices are mounted.
static bool cdc_mounted[CFG_TUH_CDC];

void cdc_init(void)
{
    for (int i = 0; i < CDC_MAX_DESC; i++)
        cdc_desc[i].tuh_idx = CDC_IDX_INVALID;
    memset(cdc_mounted, 0, sizeof(cdc_mounted));
}

void cdc_task(void)
{
    // Flush pending TX data for all mounted CDC interfaces.
    for (uint8_t idx = 0; idx < CFG_TUH_CDC; idx++)
    {
        if (cdc_mounted[idx])
            tuh_cdc_write_flush(idx);
    }
}

int cdc_count(void)
{
    int count = 0;
    for (uint8_t idx = 0; idx < CFG_TUH_CDC; idx++)
        if (cdc_mounted[idx])
            count++;
    return count;
}

// Open a CDC device by name. Names are "COM0:" through "COM7:".
// Returns descriptor index on success, -1 on failure.
int cdc_open(const char *name)
{
    // Parse COMn: prefix
    if (strncasecmp(name, "COM", 3) != 0)
        return -1;
    if (!isdigit((unsigned char)name[3]))
        return -1;
    uint8_t tuh_idx = name[3] - '0';
    if (name[4] != ':' && name[4] != '\0')
        return -1;

    if (tuh_idx >= CFG_TUH_CDC)
        return -1;
    if (!cdc_mounted[tuh_idx])
        return -1;

    // Check not already opened by another descriptor
    for (int i = 0; i < CDC_MAX_DESC; i++)
        if (cdc_desc[i].tuh_idx == tuh_idx)
            return -1;

    // Find a free descriptor slot
    for (int i = 0; i < CDC_MAX_DESC; i++)
    {
        if (cdc_desc[i].tuh_idx == CDC_IDX_INVALID)
        {
            cdc_desc[i].tuh_idx = tuh_idx;
            DBG("CDC open COM%d: desc=%d\n", tuh_idx, i);
            return i;
        }
    }
    return -1; // No free descriptor slots
}

bool cdc_close(int desc_idx)
{
    if (desc_idx < 0 || desc_idx >= CDC_MAX_DESC)
        return false;
    if (cdc_desc[desc_idx].tuh_idx == CDC_IDX_INVALID)
        return false;
    DBG("CDC close desc=%d tuh_idx=%d\n", desc_idx, cdc_desc[desc_idx].tuh_idx);
    cdc_desc[desc_idx].tuh_idx = CDC_IDX_INVALID;
    return true;
}

int cdc_rx(int desc_idx, char *buf, int buf_size)
{
    if (desc_idx < 0 || desc_idx >= CDC_MAX_DESC)
        return -1;
    uint8_t tuh_idx = cdc_desc[desc_idx].tuh_idx;
    if (tuh_idx == CDC_IDX_INVALID)
        return -1;
    if (!cdc_mounted[tuh_idx])
        return -1;
    uint32_t count = tuh_cdc_read(tuh_idx, buf, (uint32_t)buf_size);
    return (int)count;
}

int cdc_tx(int desc_idx, const char *buf, int buf_size)
{
    if (desc_idx < 0 || desc_idx >= CDC_MAX_DESC)
        return -1;
    uint8_t tuh_idx = cdc_desc[desc_idx].tuh_idx;
    if (tuh_idx == CDC_IDX_INVALID)
        return -1;
    if (!cdc_mounted[tuh_idx])
        return -1;
    uint32_t count = tuh_cdc_write(tuh_idx, buf, (uint32_t)buf_size);
    tuh_cdc_write_flush(tuh_idx);
    return (int)count;
}

// TinyUSB CDC host callbacks

void tuh_cdc_mount_cb(uint8_t idx)
{
    tuh_itf_info_t info;
    tuh_cdc_itf_get_info(idx, &info);
    DBG("CDC mounted: idx=%d dev_addr=%d\n", idx, info.daddr);
    if (idx < CFG_TUH_CDC)
        cdc_mounted[idx] = true;
}

void tuh_cdc_umount_cb(uint8_t idx)
{
    DBG("CDC unmounted: idx=%d\n", idx);
    if (idx < CFG_TUH_CDC)
    {
        cdc_mounted[idx] = false;
        // Close any descriptors referencing this device
        for (int i = 0; i < CDC_MAX_DESC; i++)
            if (cdc_desc[i].tuh_idx == idx)
                cdc_desc[i].tuh_idx = CDC_IDX_INVALID;
    }
}

void tuh_cdc_rx_cb(uint8_t idx)
{
    (void)idx;
    // Data is buffered by TinyUSB; polled via cdc_rx().
}

void tuh_cdc_tx_complete_cb(uint8_t idx)
{
    (void)idx;
}
