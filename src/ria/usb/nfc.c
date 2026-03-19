/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "usb/nfc.h"
#include "usb/vcp.h"
#include "aud/bel.h"
#include "str/str.h"
#include "sys/cfg.h"
#include <tusb.h>
#include <stdio.h>
#include <string.h>
#include <pico/time.h>

#define DEBUG_RIA_USB_NFC

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_NFC)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// PN532 command codes
#define PN532_PREAMBLE 0x00
#define PN532_STARTCODE1 0x00
#define PN532_STARTCODE2 0xFF
#define PN532_POSTAMBLE 0x00
#define PN532_HOST_TO_PN532 0xD4
#define PN532_PN532_TO_HOST 0xD5

#define PN532_CMD_GETFIRMWAREVERSION 0x02
#define PN532_CMD_SAMCONFIGURATION 0x14
#define PN532_CMD_INLISTPASSIVETARGET 0x4A
#define PN532_CMD_INDATAEXCHANGE 0x40

#define PN532_ACK_FRAME_SIZE 6
#define PN532_MAX_FRAME_SIZE 64
#define PN532_WAKEUP_SIZE 16

// Timeouts
#define NFC_ACK_TIMEOUT_MS 100
#define NFC_RESPONSE_TIMEOUT_MS 500
#define NFC_POLL_INTERVAL_MS 500
#define NFC_RETRY_INTERVAL_MS 2000

// NDEF Type 2 Tag commands (NTAG/Mifare Ultralight)
#define NDEF_READ_CMD 0x30
#define NDEF_TLV_NULL 0x00
#define NDEF_TLV_NDEF_MESSAGE 0x03
#define NDEF_TLV_TERMINATOR 0xFE

static const char *const nfc_state_names[] = {
    "OFF",
    "WAIT_DEVICE",
    "SCAN_OPEN",
    "SCAN_GFV_TX",
    "SCAN_PROBE_ACK",
    "SCAN_PROBE_RX",
    "SCAN_CLOSE",
    "SAM_TX",
    "SAM_ACK",
    "SAM_RX",
    "IDLE",
    "POLL_TX",
    "POLL_ACK",
    "POLL_RX",
    "READ_TX",
    "READ_ACK",
    "READ_RX",
    "CARD_PRESENT",
    "DETECT_REMOVAL_TX",
    "DETECT_REMOVAL_ACK",
    "DETECT_REMOVAL_RX",
    "CLOSING",
};

static enum {
    NFC_OFF,
    NFC_WAIT_DEVICE,
    NFC_SCAN_OPEN,
    NFC_SCAN_GFV_TX,
    NFC_SCAN_PROBE_ACK,
    NFC_SCAN_PROBE_RX,
    NFC_SCAN_CLOSE,
    NFC_SAM_TX,
    NFC_SAM_ACK,
    NFC_SAM_RX,
    NFC_IDLE,
    NFC_POLL_TX,
    NFC_POLL_ACK,
    NFC_POLL_RX,
    NFC_READ_TX,
    NFC_READ_ACK,
    NFC_READ_RX,
    NFC_CARD_PRESENT,
    NFC_DETECT_REMOVAL_TX,
    NFC_DETECT_REMOVAL_ACK,
    NFC_DETECT_REMOVAL_RX,
    NFC_CLOSING,
} nfc_state;

static uint8_t nfc_enabled;
static int nfc_desc = -1;
static uint8_t nfc_scan_idx;
static absolute_time_t nfc_timeout;

// Transport layer: non-blocking TX/RX with position tracking.
static uint8_t nfc_tx_buf[PN532_WAKEUP_SIZE + PN532_MAX_FRAME_SIZE];
static size_t nfc_tx_len;
static size_t nfc_ack_end;
static size_t nfc_tx_pos;
static uint8_t nfc_rx_buf[PN532_MAX_FRAME_SIZE];
static size_t nfc_rx_pos;

// NDEF read state
static uint8_t nfc_read_page;
static uint8_t nfc_ndef_buf[128];
static size_t nfc_ndef_len;

static void nfc_set_state(int new_state)
{
    if (nfc_state != new_state)
    {
        if (!(nfc_state == NFC_POLL_TX || new_state == NFC_POLL_TX ||
              nfc_state == NFC_POLL_RX || new_state == NFC_POLL_RX))
            DBG("[%6lu] NFC: %s -> %s\n",
                (unsigned long)to_ms_since_boot(get_absolute_time()),
                nfc_state_names[nfc_state],
                nfc_state_names[new_state]);
        nfc_state = new_state;
    }
}

// --- Transport layer ---

// Build a PN532 frame into TX buffer and reset TX position.
static size_t nfc_build_frame(uint8_t cmd, const uint8_t *data, size_t data_len)
{
    nfc_tx_buf[0] = 0x55;
    memset(nfc_tx_buf + 1, 0x00, PN532_WAKEUP_SIZE - 1);
    size_t fi = PN532_WAKEUP_SIZE;
    uint8_t len = (uint8_t)(data_len + 2); // TFI + cmd + data
    nfc_tx_buf[fi++] = PN532_PREAMBLE;
    nfc_tx_buf[fi++] = PN532_STARTCODE1;
    nfc_tx_buf[fi++] = PN532_STARTCODE2;
    nfc_tx_buf[fi++] = len;
    nfc_tx_buf[fi++] = (uint8_t)(~len + 1); // LCS
    nfc_tx_buf[fi++] = PN532_HOST_TO_PN532;
    nfc_tx_buf[fi++] = cmd;
    uint8_t dcs = PN532_HOST_TO_PN532 + cmd;
    for (size_t i = 0; i < data_len; i++)
    {
        nfc_tx_buf[fi++] = data[i];
        dcs += data[i];
    }
    nfc_tx_buf[fi++] = (uint8_t)(~dcs + 1); // DCS
    nfc_tx_buf[fi++] = PN532_POSTAMBLE;
    nfc_tx_len = fi;
    nfc_tx_pos = 0;
    return fi;
}

// Non-blocking send. Returns: 1 = complete, 0 = pending, -1 = error.
// Call repeatedly from the same state until complete or error.
static int nfc_send(void)
{
    if (nfc_tx_pos >= nfc_tx_len)
        return 1;
    uint32_t bytes_written = 0;
    api_errno err;
    std_rw_result result = vcp_std_write(nfc_desc,
                                         (const char *)nfc_tx_buf + nfc_tx_pos,
                                         (uint32_t)(nfc_tx_len - nfc_tx_pos),
                                         &bytes_written, &err);
    if (result != STD_OK)
        return -1;
    // if (bytes_written > 0)
    // {
    //     uint32_t ts = (uint32_t)to_ms_since_boot(get_absolute_time());
    //     DBG("[%6lu] NFC TX:", (unsigned long)ts);
    //     for (uint32_t i = 0; i < bytes_written; i++)
    //         printf(" %02X", nfc_tx_buf[nfc_tx_pos + i]);
    //     printf("\n");
    // }
    nfc_tx_pos += bytes_written;
    return (nfc_tx_pos >= nfc_tx_len) ? 1 : 0;
}

// Non-blocking receive. Appends to RX buffer. Returns bytes read this call.
static uint32_t nfc_recv(void)
{
    uint32_t bytes_read = 0;
    api_errno err;
    vcp_std_read(nfc_desc, (char *)nfc_rx_buf + nfc_rx_pos,
                 (uint32_t)(sizeof(nfc_rx_buf) - nfc_rx_pos), &bytes_read, &err);
    // if (bytes_read > 0)
    // {
    //     uint32_t ts = (uint32_t)to_ms_since_boot(get_absolute_time());
    //     DBG("[%6lu] NFC RX:", (unsigned long)ts);
    //     for (uint32_t i = 0; i < bytes_read; i++)
    //         printf(" %02X", nfc_rx_buf[nfc_rx_pos + i]);
    //     printf("\n");
    // }
    nfc_rx_pos += bytes_read;
    return bytes_read;
}

// Check if we received the 6-byte ACK frame anywhere in the buffer.
// The PN532 spec allows one or more 0x00 preamble bytes before the ACK.
// Sets nfc_ack_end to the buffer position immediately after the ACK.
static bool nfc_check_ack(void)
{
    static const uint8_t ack[PN532_ACK_FRAME_SIZE] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
    for (size_t i = 0; i + PN532_ACK_FRAME_SIZE <= nfc_rx_pos; i++)
    {
        if (memcmp(nfc_rx_buf + i, ack, PN532_ACK_FRAME_SIZE) == 0)
        {
            nfc_ack_end = i + PN532_ACK_FRAME_SIZE;
            return true;
        }
    }
    return false;
}

// Parse a response frame after ACK. Returns pointer to response data
// (after TFI+cmd_response) or NULL if incomplete/invalid.
// nfc_ack_end must have been set by a successful nfc_check_ack() call.
static const uint8_t *nfc_parse_response(size_t *resp_len)
{
    if (nfc_rx_pos < nfc_ack_end + 7)
        return NULL;
    const uint8_t *f = nfc_rx_buf + nfc_ack_end;
    size_t remain = nfc_rx_pos - nfc_ack_end;
    if (f[0] != 0x00 || f[1] != 0x00 || f[2] != 0xFF)
        return NULL;
    uint8_t len = f[3];
    uint8_t lcs = f[4];
    if ((uint8_t)(len + lcs) != 0)
        return NULL;
    if (remain < (size_t)(len + 7))
        return NULL;
    if (f[5] != PN532_PN532_TO_HOST)
        return NULL;
    uint8_t dcs = 0;
    for (uint8_t i = 0; i < len; i++)
        dcs += f[5 + i];
    if ((uint8_t)(dcs + f[5 + len]) != 0)
        return NULL;
    if (len < 2)
        return NULL;
    *resp_len = len - 2;
    return &f[7];
}

static bool nfc_timed_out(void)
{
    return absolute_time_diff_us(get_absolute_time(), nfc_timeout) < 0;
}

static void nfc_close_device(void)
{
    if (nfc_desc >= 0)
    {
        api_errno err;
        vcp_std_close(nfc_desc, &err);
        nfc_desc = -1;
    }
}

// --- Configuration ---

static void nfc_set_config(uint8_t val)
{
    switch (val)
    {
    case 0:
        nfc_close_device();
        nfc_set_state(NFC_OFF);
        break;
    case 1:
        if (nfc_state == NFC_OFF)
            nfc_set_state(NFC_WAIT_DEVICE);
        break;
    case 2:
        if (nfc_desc >= 0)
            break;
        nfc_scan_idx = 0;
        nfc_set_state(NFC_SCAN_OPEN);
        break;
    case 86:
        nfc_close_device();
        vcp_set_nfc_device_name("");
        nfc_set_state(NFC_OFF);
        break;
    }
}

// --- Public API ---

void nfc_load_enabled(const char *str)
{
    str_parse_uint8(&str, &nfc_enabled);
    if (nfc_enabled > 1)
        nfc_enabled = 0;
    if (nfc_enabled)
        nfc_set_config(1);
}

bool nfc_set_enabled(uint8_t val)
{
    if (val > 2 && val != 86)
        return false;
    nfc_set_config(val);
    if (val == 86)
        val = 0;
    if (val > 1)
        val = 1;
    if (nfc_enabled != val)
    {
        nfc_enabled = val;
        cfg_save();
    }
    return true;
}

uint8_t nfc_get_enabled(void)
{
    return nfc_enabled;
}

// --- State machine task ---

void nfc_task(void)
{
    switch (nfc_state)
    {
    case NFC_OFF:
        break;

    case NFC_WAIT_DEVICE:
    {
        nfc_desc = vcp_nfc_open();
        if (nfc_desc >= 0)
        {
            DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
            DBG("opened VCP desc %d\n", nfc_desc);
            nfc_tx_len = 0;
            nfc_tx_pos = 0;
            nfc_set_state(NFC_SAM_TX);
        }
        break;
    }

    case NFC_SCAN_OPEN:
    {
        if (nfc_scan_idx >= CFG_TUH_CDC)
        {
            DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
            DBG("no PN532 found\n");
            nfc_set_state(NFC_WAIT_DEVICE);
            break;
        }
        char name[8];
        snprintf(name, sizeof(name), "VCP%d:", nfc_scan_idx);
        api_errno err;
        nfc_desc = vcp_std_open(name, 0, &err);
        if (nfc_desc >= 0)
        {
            DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
            DBG("scanning %s\n", name);
            nfc_tx_len = 0;
            nfc_tx_pos = 0;
            nfc_set_state(NFC_SCAN_GFV_TX);
        }
        else
        {
            nfc_scan_idx++;
        }
        break;
    }

    case NFC_SCAN_GFV_TX:
    {
        if (nfc_tx_len == 0)
            nfc_build_frame(PN532_CMD_GETFIRMWAREVERSION, NULL, 0);
        int rc = nfc_send();
        if (rc < 0)
        {
            nfc_set_state(NFC_SCAN_CLOSE);
            break;
        }
        if (rc == 1)
        {
            nfc_rx_pos = 0;
            nfc_ack_end = 0;
            nfc_timeout = make_timeout_time_ms(NFC_ACK_TIMEOUT_MS);
            nfc_set_state(NFC_SCAN_PROBE_ACK);
        }
        break;
    }

    case NFC_SCAN_PROBE_ACK:
        nfc_recv();
        if (nfc_check_ack())
        {
            nfc_timeout = make_timeout_time_ms(NFC_RESPONSE_TIMEOUT_MS);
            nfc_set_state(NFC_SCAN_PROBE_RX);
        }
        else if (nfc_timed_out())
        {
            nfc_set_state(NFC_SCAN_CLOSE);
        }
        break;

    case NFC_SCAN_PROBE_RX:
    {
        nfc_recv();
        size_t resp_len;
        const uint8_t *resp = nfc_parse_response(&resp_len);
        if (resp && resp_len >= 3)
        {
            DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
            DBG("PN532 found IC=0x%02X Ver=%d.%d\n",
                resp[0], resp[1], resp[2]);
            char name[8];
            snprintf(name, sizeof(name), "VCP%d:", nfc_scan_idx);
            vcp_set_nfc_device_name(name);
            nfc_tx_len = 0;
            nfc_tx_pos = 0;
            nfc_timeout = make_timeout_time_ms(0);
            nfc_set_state(NFC_SAM_TX);
        }
        else if (nfc_timed_out())
        {
            nfc_set_state(NFC_SCAN_CLOSE);
        }
        break;
    }

    case NFC_SCAN_CLOSE:
        nfc_close_device();
        nfc_scan_idx++;
        nfc_set_state(NFC_SCAN_OPEN);
        break;

    case NFC_SAM_TX:
    {
        if (nfc_tx_len == 0)
        {
            uint8_t sam_data[] = {0x01, 0x14, 0x01};
            nfc_build_frame(PN532_CMD_SAMCONFIGURATION, sam_data, sizeof(sam_data));
        }
        int rc = nfc_send();
        if (rc < 0)
        {
            DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
            DBG("SAM send failed\n");
            nfc_close_device();
            nfc_set_state(NFC_WAIT_DEVICE);
            nfc_timeout = make_timeout_time_ms(NFC_RETRY_INTERVAL_MS);
        }
        else if (rc == 1)
        {
            nfc_rx_pos = 0;
            nfc_ack_end = 0;
            nfc_timeout = make_timeout_time_ms(NFC_ACK_TIMEOUT_MS);
            nfc_set_state(NFC_SAM_ACK);
        }
        break;
    }

    case NFC_SAM_ACK:
        nfc_recv();
        if (nfc_check_ack())
        {
            nfc_timeout = make_timeout_time_ms(NFC_RESPONSE_TIMEOUT_MS);
            nfc_set_state(NFC_SAM_RX);
        }
        else if (nfc_timed_out())
        {
            DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
            DBG("SAM ACK timeout\n");
            nfc_close_device();
            nfc_set_state(NFC_WAIT_DEVICE);
            nfc_timeout = make_timeout_time_ms(NFC_RETRY_INTERVAL_MS);
        }
        break;

    case NFC_SAM_RX:
    {
        nfc_recv();
        size_t resp_len;
        const uint8_t *resp = nfc_parse_response(&resp_len);
        if (resp)
        {
            DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
            DBG("SAM configured\n");
            nfc_set_state(NFC_IDLE);
            nfc_timeout = make_timeout_time_ms(NFC_POLL_INTERVAL_MS);
        }
        else if (nfc_timed_out())
        {
            DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
            DBG("SAM response timeout\n");
            nfc_close_device();
            nfc_set_state(NFC_WAIT_DEVICE);
            nfc_timeout = make_timeout_time_ms(NFC_RETRY_INTERVAL_MS);
        }
        break;
    }

    case NFC_IDLE:
        if (nfc_timed_out())
        {
            nfc_tx_len = 0;
            nfc_tx_pos = 0;
            nfc_set_state(NFC_POLL_TX);
        }
        break;

    case NFC_POLL_TX:
    {
        if (nfc_tx_len == 0)
        {
            uint8_t poll_data[] = {0x01, 0x00};
            nfc_build_frame(PN532_CMD_INLISTPASSIVETARGET, poll_data, sizeof(poll_data));
        }
        int rc = nfc_send();
        if (rc < 0)
        {
            DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
            DBG("poll send failed\n");
            nfc_close_device();
            nfc_set_state(NFC_WAIT_DEVICE);
            nfc_timeout = make_timeout_time_ms(NFC_RETRY_INTERVAL_MS);
        }
        else if (rc == 1)
        {
            nfc_rx_pos = 0;
            nfc_ack_end = 0;
            nfc_timeout = make_timeout_time_ms(NFC_ACK_TIMEOUT_MS);
            nfc_set_state(NFC_POLL_ACK);
        }
        break;
    }

    case NFC_POLL_ACK:
        nfc_recv();
        if (nfc_check_ack())
        {
            nfc_timeout = make_timeout_time_ms(NFC_RESPONSE_TIMEOUT_MS);
            nfc_set_state(NFC_POLL_RX);
        }
        else if (nfc_timed_out())
        {
            DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
            DBG("poll ACK timeout\n");
            nfc_close_device();
            nfc_set_state(NFC_WAIT_DEVICE);
            nfc_timeout = make_timeout_time_ms(NFC_RETRY_INTERVAL_MS);
        }
        break;

    case NFC_POLL_RX:
    {
        nfc_recv();
        size_t resp_len;
        const uint8_t *resp = nfc_parse_response(&resp_len);
        if (resp)
        {
            if (resp_len >= 1 && resp[0] > 0)
            {
                DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
                DBG("detected\n");
                aud_bel_add(&bel_nfc_detect);
                nfc_read_page = 4;
                nfc_ndef_len = 0;
                nfc_tx_len = 0;
                nfc_tx_pos = 0;
                nfc_set_state(NFC_READ_TX);
            }
            else
            {
                nfc_set_state(NFC_IDLE);
                nfc_timeout = make_timeout_time_ms(NFC_POLL_INTERVAL_MS);
            }
        }
        else if (nfc_timed_out())
        {
            nfc_set_state(NFC_IDLE);
            nfc_timeout = make_timeout_time_ms(NFC_POLL_INTERVAL_MS);
        }
        break;
    }

    case NFC_READ_TX:
    {
        if (nfc_tx_len == 0)
        {
            uint8_t read_data[] = {0x01, NDEF_READ_CMD, nfc_read_page};
            nfc_build_frame(PN532_CMD_INDATAEXCHANGE, read_data, sizeof(read_data));
        }
        int rc = nfc_send();
        if (rc < 0)
        {
            DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
            DBG("read ndef fail\n");
            aud_bel_add(&bel_nfc_fail);
            nfc_set_state(NFC_IDLE);
            nfc_timeout = make_timeout_time_ms(NFC_POLL_INTERVAL_MS);
        }
        else if (rc == 1)
        {
            nfc_rx_pos = 0;
            nfc_ack_end = 0;
            nfc_timeout = make_timeout_time_ms(NFC_ACK_TIMEOUT_MS);
            nfc_set_state(NFC_READ_ACK);
        }
        break;
    }

    case NFC_READ_ACK:
        nfc_recv();
        if (nfc_check_ack())
        {
            nfc_timeout = make_timeout_time_ms(NFC_RESPONSE_TIMEOUT_MS);
            nfc_set_state(NFC_READ_RX);
        }
        else if (nfc_timed_out())
        {
            DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
            DBG("read ndef fail\n");
            aud_bel_add(&bel_nfc_fail);
            nfc_set_state(NFC_IDLE);
            nfc_timeout = make_timeout_time_ms(NFC_POLL_INTERVAL_MS);
        }
        break;

    case NFC_READ_RX:
    {
        nfc_recv();
        size_t resp_len;
        const uint8_t *resp = nfc_parse_response(&resp_len);
        if (resp)
        {
            if (resp_len >= 1 && resp[0] == 0x00 && resp_len > 1)
            {
                size_t data_len = resp_len - 1;
                const uint8_t *data = resp + 1;
                for (size_t i = 0; i < data_len && nfc_ndef_len < sizeof(nfc_ndef_buf); i++)
                    nfc_ndef_buf[nfc_ndef_len++] = data[i];

                bool found_terminator = false;
                for (size_t i = 0; i < nfc_ndef_len; i++)
                {
                    if (nfc_ndef_buf[i] == NDEF_TLV_TERMINATOR)
                    {
                        found_terminator = true;
                        break;
                    }
                }

                if (found_terminator || nfc_ndef_len >= sizeof(nfc_ndef_buf))
                {
                    DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
                    DBG("read ndef success (%u bytes)\n", (unsigned)nfc_ndef_len);
                    aud_bel_add(&bel_nfc_success_1);
                    nfc_set_state(NFC_CARD_PRESENT);
                    nfc_timeout = make_timeout_time_ms(NFC_POLL_INTERVAL_MS);
                }
                else
                {
                    nfc_read_page += 4;
                    nfc_tx_len = 0;
                    nfc_tx_pos = 0;
                    nfc_set_state(NFC_READ_TX);
                }
            }
            else
            {
                DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
                DBG("read ndef fail\n");
                aud_bel_add(&bel_nfc_fail);
                nfc_set_state(NFC_CARD_PRESENT);
                nfc_timeout = make_timeout_time_ms(NFC_POLL_INTERVAL_MS);
            }
        }
        else if (nfc_timed_out())
        {
            DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
            DBG("read ndef fail\n");
            aud_bel_add(&bel_nfc_fail);
            nfc_set_state(NFC_IDLE);
            nfc_timeout = make_timeout_time_ms(NFC_POLL_INTERVAL_MS);
        }
        break;
    }

    case NFC_CARD_PRESENT:
        if (nfc_timed_out())
        {
            nfc_tx_len = 0;
            nfc_tx_pos = 0;
            nfc_set_state(NFC_DETECT_REMOVAL_TX);
        }
        break;

    case NFC_DETECT_REMOVAL_TX:
    {
        if (nfc_tx_len == 0)
        {
            uint8_t poll_data[] = {0x01, 0x00};
            nfc_build_frame(PN532_CMD_INLISTPASSIVETARGET, poll_data, sizeof(poll_data));
        }
        int rc = nfc_send();
        if (rc < 0)
        {
            nfc_close_device();
            nfc_set_state(NFC_WAIT_DEVICE);
            nfc_timeout = make_timeout_time_ms(NFC_RETRY_INTERVAL_MS);
        }
        else if (rc == 1)
        {
            nfc_rx_pos = 0;
            nfc_ack_end = 0;
            nfc_timeout = make_timeout_time_ms(NFC_ACK_TIMEOUT_MS);
            nfc_set_state(NFC_DETECT_REMOVAL_ACK);
        }
        break;
    }

    case NFC_DETECT_REMOVAL_ACK:
        nfc_recv();
        if (nfc_check_ack())
        {
            nfc_timeout = make_timeout_time_ms(NFC_RESPONSE_TIMEOUT_MS);
            nfc_set_state(NFC_DETECT_REMOVAL_RX);
        }
        else if (nfc_timed_out())
        {
            nfc_close_device();
            nfc_set_state(NFC_WAIT_DEVICE);
            nfc_timeout = make_timeout_time_ms(NFC_RETRY_INTERVAL_MS);
        }
        break;

    case NFC_DETECT_REMOVAL_RX:
    {
        nfc_recv();
        size_t resp_len;
        const uint8_t *resp = nfc_parse_response(&resp_len);
        if (resp)
        {
            if (resp_len >= 1 && resp[0] > 0)
            {
                nfc_set_state(NFC_CARD_PRESENT);
                nfc_timeout = make_timeout_time_ms(NFC_POLL_INTERVAL_MS);
            }
            else
            {
                DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
                DBG("removed\n");
                aud_bel_add(&bel_nfc_remove);
                nfc_set_state(NFC_IDLE);
                nfc_timeout = make_timeout_time_ms(NFC_POLL_INTERVAL_MS);
            }
        }
        else if (nfc_timed_out())
        {
            DBG("[%6lu] NFC: ", (unsigned long)to_ms_since_boot(get_absolute_time()));
            DBG("removed\n");
            aud_bel_add(&bel_nfc_remove);
            nfc_set_state(NFC_IDLE);
            nfc_timeout = make_timeout_time_ms(NFC_POLL_INTERVAL_MS);
        }
        break;
    }

    case NFC_CLOSING:
        nfc_close_device();
        nfc_set_state(NFC_OFF);
        break;
    }
}
