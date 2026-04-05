/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/pro.h"
#include "usb/nfc.h"
#include "usb/vcp.h"
#include "aud/bel.h"
#include "str/str.h"
#include "sys/cfg.h"
#include <tusb.h>
#include <stdio.h>
#include <string.h>
#include <pico/time.h>

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_NFC)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// NFC API command opcodes (written by 6502)
#define NFC_CMD_WRITE 0x01
#define NFC_CMD_CANCEL 0x02
#define NFC_CMD_READ 0x03
#define NFC_CMD_SUCCESS1 0x04
#define NFC_CMD_SUCCESS2 0x05
#define NFC_CMD_ERROR 0x06

// NFC API response types (read by 6502)
#define NFC_RESP_READ 0x01
#define NFC_RESP_WRITE 0x02
#define NFC_RESP_NO_READER 0x03
#define NFC_RESP_NO_CARD 0x04
#define NFC_RESP_CARD_INSERTED 0x05
#define NFC_RESP_CARD_READY 0x06

// Max tag data area for NTAG216
#define NFC_TAG_BUF_SIZE 888

// Timeouts
#define NFC_ACK_TIMEOUT_MS 50
#define NFC_RESPONSE_TIMEOUT_MS 100
#define NFC_POLL_INTERVAL_MS 200
#define NFC_RETRY_INTERVAL_MS 2000

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

// NDEF Type 2 Tag commands (NTAG/Mifare Ultralight)
#define NDEF_READ_CMD 0x30
#define NDEF_WRITE_CMD 0xA2
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
    "CC_TX",
    "CC_ACK",
    "CC_RX",
    "READ_TX",
    "READ_ACK",
    "READ_RX",
    "CARD_PRESENT",
    "DETECT_REMOVAL_TX",
    "DETECT_REMOVAL_ACK",
    "DETECT_REMOVAL_RX",
    "TAG_WRITE_TX",
    "TAG_WRITE_ACK",
    "TAG_WRITE_RX",
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
    NFC_CC_TX,
    NFC_CC_ACK,
    NFC_CC_RX,
    NFC_READ_TX,
    NFC_READ_ACK,
    NFC_READ_RX,
    NFC_CARD_PRESENT,
    NFC_DETECT_REMOVAL_TX,
    NFC_DETECT_REMOVAL_ACK,
    NFC_DETECT_REMOVAL_RX,
    NFC_TAG_WRITE_TX,
    NFC_TAG_WRITE_ACK,
    NFC_TAG_WRITE_RX,
    NFC_CLOSING,
} nfc_state;

static uint8_t nfc_enabled;
static int nfc_desc = -1;
static uint8_t nfc_scan_idx;
static absolute_time_t nfc_timeout;
static bool nfc_card_inserted;

// Transport layer: non-blocking TX/RX with position tracking.
static uint8_t nfc_tx_buf[PN532_WAKEUP_SIZE + PN532_MAX_FRAME_SIZE];
static size_t nfc_tx_len;
static size_t nfc_ack_end;
static size_t nfc_tx_pos;
static uint8_t nfc_rx_buf[PN532_MAX_FRAME_SIZE];
static size_t nfc_rx_pos;

// Tag data read state, up to NTAG216 size
static uint8_t nfc_read_page;
static uint8_t nfc_tag_buf[NFC_TAG_BUF_SIZE];
static size_t nfc_tag_len;
static bool nfc_tag_ready;

// 6502 API state
static bool nfc_api_open;
static uint8_t nfc_floor_sent;
static uint8_t nfc_read_buf[2 + NFC_TAG_BUF_SIZE]; // len +  raw tag data
static size_t nfc_read_len;
static size_t nfc_read_pos; // drain position through header + payload
static bool nfc_write_response;

// Write staging
static uint8_t nfc_write_buf[NFC_TAG_BUF_SIZE];
static size_t nfc_write_len;
static size_t nfc_write_expected;
static bool nfc_write_armed;
static bool nfc_write_failed;
static bool nfc_read_armed;
static bool nfc_write_accumulating;
static uint8_t nfc_write_cmd_pos;
static uint8_t nfc_write_start_page;
static uint8_t nfc_write_page;
static size_t nfc_write_pos;

static void nfc_goto(int new_state, uint32_t ms)
{
    if (nfc_state != new_state)
    {
        if (!(nfc_state == NFC_POLL_TX || new_state == NFC_POLL_TX ||
              nfc_state == NFC_POLL_RX || new_state == NFC_POLL_RX))
            DBG("NFC: [%6lu] %s -> %s\n",
                (unsigned long)to_ms_since_boot(get_absolute_time()),
                nfc_state_names[nfc_state],
                nfc_state_names[new_state]);
        if (new_state == NFC_OFF || new_state == NFC_WAIT_DEVICE ||
            new_state == NFC_IDLE || new_state == NFC_CLOSING)
        {
            nfc_card_inserted = false;
            nfc_write_failed = false;
            nfc_tag_ready = false;
        }
        nfc_state = new_state;
    }
    nfc_timeout = make_timeout_time_ms(ms);
}

// Build a PN532 frame into TX buffer and reset TX position.
static size_t nfc_build_frame(uint8_t cmd, const uint8_t *data, size_t data_len)
{
    // 8 overhead bytes (preamble/start/len/lcs/tfi/cmd/dcs/postamble) + wakeup
    if (data_len + 2 > 255 || PN532_WAKEUP_SIZE + data_len + 8 > sizeof(nfc_tx_buf))
        return 0; // TODO silently fail? maybe assert?
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
    nfc_tx_pos += bytes_written;
    return (nfc_tx_pos >= nfc_tx_len) ? 1 : 0;
}

// Non-blocking receive. Appends to RX buffer. Returns bytes read this call.
static uint32_t nfc_recv(void)
{
    if (nfc_rx_pos >= sizeof(nfc_rx_buf))
        return 0; // todo choke rx?
    uint32_t bytes_read = 0;
    api_errno err;
    vcp_std_read(nfc_desc, (char *)nfc_rx_buf + nfc_rx_pos,
                 (uint32_t)(sizeof(nfc_rx_buf) - nfc_rx_pos), &bytes_read, &err);
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
    nfc_ack_end = 0;
    return false;
}

// Parse a response frame after ACK. Returns pointer to response data
// (after TFI+cmd_response) or NULL if incomplete/invalid.
// nfc_ack_end must have been set by a successful nfc_check_ack() call.
// cmd is the command that was sent; the response byte must equal cmd+1.
static const uint8_t *nfc_parse_response(uint8_t cmd, size_t *resp_len)
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
    if (len < 2)
        return NULL;
    if (f[6] != (uint8_t)(cmd + 1))
        return NULL;
    uint8_t dcs = 0;
    for (uint8_t i = 0; i < len; i++)
        dcs += f[5 + i];
    if ((uint8_t)(dcs + f[5 + len]) != 0)
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

static void nfc_recover(void)
{
    nfc_close_device();
    nfc_goto(NFC_WAIT_DEVICE, NFC_RETRY_INTERVAL_MS);
}

static void nfc_write_fail(void)
{
    nfc_write_failed = true;
    bel_add(&bel_nfc_fail);
    nfc_goto(NFC_CARD_PRESENT, NFC_POLL_INTERVAL_MS);
}

static void nfc_read_complete(void)
{
    nfc_tag_ready = true;
    nfc_goto(NFC_CARD_PRESENT, NFC_POLL_INTERVAL_MS);
    if (nfc_tag_len > 0 && !nfc_api_open)
        pro_nfc(nfc_tag_buf, nfc_tag_len);
}

static void nfc_begin_receive(int ack_state)
{
    nfc_rx_pos = 0;
    nfc_ack_end = 0;
    nfc_goto(ack_state, NFC_ACK_TIMEOUT_MS);
}

static void nfc_start_tx(int tx_state)
{
    // Drain stale FIFO bytes before issuing a new command.
    uint32_t drained = 0;
    api_errno err;
    vcp_std_read(nfc_desc, (char *)nfc_rx_buf, sizeof(nfc_rx_buf), &drained, &err);
    nfc_tx_len = 0;
    nfc_tx_pos = 0;
    nfc_goto(tx_state, 0);
}

static void nfc_set_config(uint8_t val)
{
    switch (val)
    {
    case 0:
        nfc_close_device();
        nfc_goto(NFC_OFF, 0);
        break;
    case 1:
        if (nfc_state == NFC_OFF)
            nfc_goto(NFC_WAIT_DEVICE, 0);
        break;
    case 2:
        if (nfc_desc >= 0)
        {
            bel_add(&bel_nfc_success_1);
            bel_add(&bel_nfc_success_2);
            break;
        }
        nfc_scan_idx = 0;
        nfc_goto(NFC_SCAN_OPEN, 0);
        break;
    case 86:
        nfc_close_device();
        vcp_set_nfc_device_name("");
        nfc_goto(NFC_OFF, 0);
        break;
    }
}

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

// Parse tag data and extract the first Well Known Text record
// into buf (NUL-terminated). Returns false if no text record is found.
bool nfc_parse_text(const uint8_t *tag_data, size_t len, char *buf, size_t buf_size)
{
    // Pages 0-3 (16 bytes) are UID/lock/CC; user data starts at page 4
    if (len <= 16)
        return false;
    tag_data += 16;
    len -= 16;

    // Walk TLV blocks to find the NDEF Message TLV (type 0x03)
    const uint8_t *msg = NULL;
    size_t msg_len = 0;
    size_t pos = 0;
    while (pos < len)
    {
        uint8_t tlv_type = tag_data[pos++];
        if (tlv_type == 0xFE) // terminator
            break;
        if (tlv_type == 0x00) // null
            continue;
        if (pos >= len)
            break;
        size_t tlv_len;
        if (tag_data[pos] == 0xFF) // three-byte length
        {
            if (pos + 2 >= len)
                break;
            tlv_len = ((size_t)tag_data[pos + 1] << 8) | tag_data[pos + 2];
            pos += 3;
        }
        else
        {
            tlv_len = tag_data[pos++];
        }
        if (pos + tlv_len > len)
            break;
        if (tlv_type == 0x03) // NDEF Message
        {
            msg = tag_data + pos;
            msg_len = tlv_len;
            break;
        }
        pos += tlv_len;
    }
    if (!msg)
        return false;

    // Find the first NDEF Well Known Type "T" (text) record
    size_t rpos = 0;
    while (rpos < msg_len)
    {
        uint8_t hdr = msg[rpos++];
        uint8_t tnf = hdr & 0x07;
        bool cf = (bool)((hdr >> 5) & 1);
        bool sr = (bool)((hdr >> 4) & 1);
        bool il = (bool)((hdr >> 3) & 1);
        if (rpos >= msg_len)
            break;
        uint8_t type_len = msg[rpos++];
        uint32_t payload_len;
        if (sr)
        {
            if (rpos >= msg_len)
                break;
            payload_len = msg[rpos++];
        }
        else
        {
            if (rpos + 4 > msg_len)
                break;
            payload_len = ((uint32_t)msg[rpos] << 24) | ((uint32_t)msg[rpos + 1] << 16) |
                          ((uint32_t)msg[rpos + 2] << 8) | msg[rpos + 3];
            rpos += 4;
        }
        uint8_t id_len = 0;
        if (il)
        {
            if (rpos >= msg_len)
                break;
            id_len = msg[rpos++];
        }
        if (rpos + type_len + id_len > msg_len)
            break;
        const uint8_t *type_data = msg + rpos;
        rpos += type_len + id_len;
        if (payload_len > msg_len - rpos)
            break;
        const uint8_t *payload = msg + rpos;
        rpos += payload_len;
        if (tnf == 0x01 && type_len == 1 && type_data[0] == 'T' && payload_len >= 1)
        {
            if (cf)
                return false; // chunked text record not supported
            uint8_t status = payload[0];
            uint8_t lang_len = status & 0x3F;
            if (payload_len <= (uint32_t)(1 + lang_len))
                continue;
            const uint8_t *txt = payload + 1 + lang_len;
            size_t txt_len = payload_len - 1 - lang_len;
            if (txt_len >= buf_size)
                txt_len = buf_size - 1;
            memcpy(buf, txt, txt_len);
            buf[txt_len] = '\0';
            return true;
        }
    }
    return false;
}

void nfc_task(void)
{
    switch (nfc_state)
    {
    case NFC_OFF:
        break;

    case NFC_WAIT_DEVICE:
    {
        if (!nfc_timed_out())
            break;
        nfc_desc = vcp_nfc_open();
        if (nfc_desc >= 0)
            nfc_start_tx(NFC_SAM_TX);
        break;
    }

    case NFC_SCAN_OPEN:
    {
        if (nfc_scan_idx >= CFG_TUH_CDC)
        {
            bel_add(&bel_nfc_fail);
            nfc_goto(NFC_WAIT_DEVICE, 0);
            break;
        }
        char name[8];
        snprintf(name, sizeof(name), "VCP%d:", nfc_scan_idx);
        api_errno err;
        nfc_desc = vcp_std_open(name, 0, &err);
        if (nfc_desc >= 0)
            nfc_start_tx(NFC_SCAN_GFV_TX);
        else
            nfc_scan_idx++;
        break;
    }

    case NFC_SCAN_GFV_TX:
    {
        if (nfc_tx_len == 0)
            nfc_build_frame(PN532_CMD_GETFIRMWAREVERSION, NULL, 0);
        int rc = nfc_send();
        if (rc < 0)
            nfc_goto(NFC_SCAN_CLOSE, 0);
        else if (rc == 1)
            nfc_begin_receive(NFC_SCAN_PROBE_ACK);
        break;
    }

    case NFC_SCAN_PROBE_ACK:
        nfc_recv();
        if (nfc_check_ack())
            nfc_goto(NFC_SCAN_PROBE_RX, NFC_RESPONSE_TIMEOUT_MS);
        else if (nfc_timed_out())
            nfc_goto(NFC_SCAN_CLOSE, 0);
        break;

    case NFC_SCAN_PROBE_RX:
    {
        nfc_recv();
        size_t resp_len;
        const uint8_t *resp = nfc_parse_response(PN532_CMD_GETFIRMWAREVERSION, &resp_len);
        if (resp && resp_len >= 3 && resp[0] == 0x32) // IC = PN532
        {
            char name[8];
            snprintf(name, sizeof(name), "VCP%d:", nfc_scan_idx);
            vcp_set_nfc_device_name(name);
            bel_add(&bel_nfc_success_1);
            bel_add(&bel_nfc_success_2);
            nfc_start_tx(NFC_SAM_TX);
        }
        else if (nfc_timed_out())
            nfc_goto(NFC_SCAN_CLOSE, 0);
        break;
    }

    case NFC_SCAN_CLOSE:
        nfc_close_device();
        nfc_scan_idx++;
        nfc_goto(NFC_SCAN_OPEN, 0);
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
            nfc_recover();
        else if (rc == 1)
            nfc_begin_receive(NFC_SAM_ACK);
        break;
    }

    case NFC_SAM_ACK:
        nfc_recv();
        if (nfc_check_ack())
            nfc_goto(NFC_SAM_RX, NFC_RESPONSE_TIMEOUT_MS);
        else if (nfc_timed_out())
            nfc_recover();
        break;

    case NFC_SAM_RX:
    {
        nfc_recv();
        size_t resp_len;
        const uint8_t *resp = nfc_parse_response(PN532_CMD_SAMCONFIGURATION, &resp_len);
        if (resp)
            nfc_goto(NFC_IDLE, NFC_POLL_INTERVAL_MS);
        else if (nfc_timed_out())
            nfc_recover();
        break;
    }

    case NFC_IDLE:
        if (nfc_timed_out())
            nfc_start_tx(NFC_POLL_TX);
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
            nfc_recover();
        else if (rc == 1)
            nfc_begin_receive(NFC_POLL_ACK);
        break;
    }

    case NFC_POLL_ACK:
        nfc_recv();
        if (nfc_check_ack())
            nfc_goto(NFC_POLL_RX, NFC_RESPONSE_TIMEOUT_MS);
        else if (nfc_timed_out())
            nfc_recover();
        break;

    case NFC_POLL_RX:
    {
        nfc_recv();
        size_t resp_len;
        const uint8_t *resp = nfc_parse_response(PN532_CMD_INLISTPASSIVETARGET, &resp_len);
        if (resp)
        {
            if (resp_len >= 1 && resp[0] > 0)
            {
                nfc_read_page = 0;
                nfc_tag_len = 0;
                nfc_tag_ready = false;
                nfc_card_inserted = true;
                nfc_write_failed = false;
                nfc_start_tx(NFC_CC_TX);
            }
            else
                nfc_goto(NFC_IDLE, NFC_POLL_INTERVAL_MS);
        }
        else if (nfc_timed_out())
            nfc_goto(NFC_IDLE, NFC_POLL_INTERVAL_MS);
        break;
    }

    case NFC_CC_TX:
    {
        if (nfc_tx_len == 0)
        {
            uint8_t read_data[] = {0x01, NDEF_READ_CMD, 3}; // page 3 = CC
            nfc_build_frame(PN532_CMD_INDATAEXCHANGE, read_data, sizeof(read_data));
        }
        int rc = nfc_send();
        if (rc < 0)
            nfc_start_tx(NFC_READ_TX);
        else if (rc == 1)
            nfc_begin_receive(NFC_CC_ACK);
        break;
    }

    case NFC_CC_ACK:
        nfc_recv();
        if (nfc_check_ack())
            nfc_goto(NFC_CC_RX, NFC_RESPONSE_TIMEOUT_MS);
        else if (nfc_timed_out())
            nfc_start_tx(NFC_READ_TX);
        break;

    case NFC_CC_RX:
    {
        nfc_recv();
        size_t resp_len;
        const uint8_t *resp = nfc_parse_response(PN532_CMD_INDATAEXCHANGE, &resp_len);
        if (resp)
        {
            if (resp_len >= 5 && resp[0] == 0x00)
            {
                const uint8_t *cc = resp + 1;
                DBG("NFC: CC magic=0x%02X ver=0x%02X size=%u(%u bytes) access=0x%02X\n",
                    cc[0], cc[1], cc[2], (unsigned)(cc[2] * 8), cc[3]);
            }
            nfc_start_tx(NFC_READ_TX);
        }
        else if (nfc_timed_out())
            nfc_start_tx(NFC_READ_TX);
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
            bel_add(&bel_nfc_fail);
            nfc_goto(NFC_IDLE, NFC_POLL_INTERVAL_MS);
        }
        else if (rc == 1)
            nfc_begin_receive(NFC_READ_ACK);
        break;
    }

    case NFC_READ_ACK:
        nfc_recv();
        if (nfc_check_ack())
            nfc_goto(NFC_READ_RX, NFC_RESPONSE_TIMEOUT_MS);
        else if (nfc_timed_out())
        {
            bel_add(&bel_nfc_fail);
            nfc_goto(NFC_IDLE, NFC_POLL_INTERVAL_MS);
        }
        break;

    case NFC_READ_RX:
    {
        nfc_recv();
        size_t resp_len;
        const uint8_t *resp = nfc_parse_response(PN532_CMD_INDATAEXCHANGE, &resp_len);
        if (resp)
        {
            if (resp_len >= 1 && resp[0] == 0x00 && resp_len > 1)
            {
                size_t data_len = resp_len - 1;
                const uint8_t *data = resp + 1;

                // All-zero page means we're past end of tag content
                bool blank = true;
                for (size_t i = 0; i < data_len; i++)
                    if (data[i])
                    {
                        blank = false;
                        break;
                    }
                if (blank)
                {
                    nfc_read_complete();
                    break;
                }

                size_t old_len = nfc_tag_len;
                for (size_t i = 0; i < data_len && nfc_tag_len < sizeof(nfc_tag_buf); i++)
                    nfc_tag_buf[nfc_tag_len++] = data[i];

                bool found_terminator = false;
                for (size_t i = old_len; i < nfc_tag_len; i++)
                    if (nfc_tag_buf[i] == NDEF_TLV_TERMINATOR)
                    {
                        found_terminator = true;
                        break;
                    }

                if (found_terminator || nfc_tag_len >= sizeof(nfc_tag_buf))
                    nfc_read_complete();
                else if (nfc_read_page <= 227)
                {
                    nfc_read_page += 4;
                    nfc_start_tx(NFC_READ_TX);
                }
                else
                    nfc_read_complete();
            }
            else
            {
                bel_add(&bel_nfc_fail);
                nfc_goto(NFC_CARD_PRESENT, NFC_POLL_INTERVAL_MS);
            }
        }
        else if (nfc_timed_out())
        {
            bel_add(&bel_nfc_fail);
            nfc_goto(NFC_IDLE, NFC_POLL_INTERVAL_MS);
        }
        break;
    }

    case NFC_CARD_PRESENT:
        if (nfc_timed_out())
        {
            if (nfc_write_armed && !nfc_write_failed)
            {
                // Pre-check: does write fit in tag?
                // tag_capacity == 0 means CC was never read; reject.
                // CC[2] is at page 3 byte 2 = tag_buf offset 14
                size_t tag_capacity = (nfc_tag_len >= 15) ? (size_t)nfc_tag_buf[14] * 8 : 0;
                if (tag_capacity == 0 || nfc_write_len > tag_capacity)
                    nfc_write_fail();
                else
                {
                    nfc_write_page = nfc_write_start_page;
                    nfc_write_pos = 0;
                    nfc_start_tx(NFC_TAG_WRITE_TX);
                }
            }
            else
                nfc_start_tx(NFC_DETECT_REMOVAL_TX);
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
            nfc_recover();
        else if (rc == 1)
            nfc_begin_receive(NFC_DETECT_REMOVAL_ACK);
        break;
    }

    case NFC_DETECT_REMOVAL_ACK:
        nfc_recv();
        if (nfc_check_ack())
            nfc_goto(NFC_DETECT_REMOVAL_RX, NFC_RESPONSE_TIMEOUT_MS);
        else if (nfc_timed_out())
            nfc_recover();
        break;

    case NFC_DETECT_REMOVAL_RX:
    {
        nfc_recv();
        size_t resp_len;
        const uint8_t *resp = nfc_parse_response(PN532_CMD_INLISTPASSIVETARGET, &resp_len);
        if (resp)
        {
            if (resp_len >= 1 && resp[0] > 0)
                nfc_goto(NFC_CARD_PRESENT, NFC_POLL_INTERVAL_MS);
            else
                nfc_goto(NFC_IDLE, NFC_POLL_INTERVAL_MS);
        }
        else if (nfc_timed_out())
            nfc_goto(NFC_IDLE, NFC_POLL_INTERVAL_MS);
        break;
    }

    case NFC_CLOSING:
        nfc_close_device();
        nfc_goto(NFC_OFF, 0);
        break;

    case NFC_TAG_WRITE_TX:
    {
        if (nfc_tx_len == 0)
        {
            uint8_t page_data[4] = {0};
            size_t remaining = (nfc_write_pos < nfc_write_len)
                                   ? nfc_write_len - nfc_write_pos
                                   : 0;
            size_t n = (remaining > 4) ? 4 : remaining;
            memcpy(page_data, nfc_write_buf + nfc_write_pos, n);
            // NTAG WRITE command: {tg, cmd, page, d0, d1, d2, d3}
            uint8_t write_data[] = {
                0x01, NDEF_WRITE_CMD, nfc_write_page,
                page_data[0], page_data[1], page_data[2], page_data[3]};
            nfc_build_frame(PN532_CMD_INDATAEXCHANGE, write_data, sizeof(write_data));
        }
        int rc = nfc_send();
        if (rc < 0)
            nfc_write_fail();
        else if (rc == 1)
            nfc_begin_receive(NFC_TAG_WRITE_ACK);
        break;
    }

    case NFC_TAG_WRITE_ACK:
        nfc_recv();
        if (nfc_check_ack())
            nfc_goto(NFC_TAG_WRITE_RX, NFC_RESPONSE_TIMEOUT_MS);
        else if (nfc_timed_out())
            nfc_write_fail();
        break;

    case NFC_TAG_WRITE_RX:
    {
        nfc_recv();
        size_t resp_len;
        const uint8_t *resp = nfc_parse_response(PN532_CMD_INDATAEXCHANGE, &resp_len);
        if (resp)
        {
            if (resp_len >= 1 && resp[0] == 0x00)
            {
                // Page written successfully, advance
                nfc_write_pos += 4;
                if (nfc_write_page < 231)
                    nfc_write_page++;
                if (nfc_write_pos >= nfc_write_len)
                {
                    nfc_write_armed = false;
                    nfc_write_response = true;
                    nfc_goto(NFC_CARD_PRESENT, NFC_POLL_INTERVAL_MS);
                }
                else
                    nfc_start_tx(NFC_TAG_WRITE_TX);
            }
            else
                nfc_write_fail();
        }
        else if (nfc_timed_out())
            nfc_write_fail();
        break;
    }
    }
}

// --- 6502 std driver interface ---

bool nfc_std_handles(const char *name)
{
    return strcasecmp(name, STR_NFC_COLON) == 0;
}

int nfc_std_open(const char *name, uint8_t flags, api_errno *err)
{
    (void)name;
    (void)flags;
    if (nfc_api_open)
    {
        *err = API_EBUSY;
        return -1;
    }
    nfc_api_open = true;
    nfc_floor_sent = 0;
    nfc_write_response = false;
    nfc_read_pos = 0;
    nfc_read_len = 0;
    nfc_read_armed = false;
    nfc_write_armed = false;
    nfc_write_accumulating = false;
    return 0;
}

int nfc_std_close(int desc, api_errno *err)
{
    (void)desc;
    (void)err;
    nfc_read_armed = false;
    nfc_write_armed = false;
    nfc_write_accumulating = false;
    nfc_write_response = false;
    nfc_api_open = false;
    return 0;
}

std_rw_result nfc_std_write(int desc, const char *buf, uint32_t count,
                            uint32_t *bytes_written, api_errno *err)
{
    (void)desc;
    (void)err;

    for (uint32_t i = 0; i < count; i++)
    {
        uint8_t b = (uint8_t)buf[i];

        if (nfc_write_accumulating)
        {
            // Streaming NFC_CMD_WRITE: page, len_lo, len_hi, then payload
            switch (nfc_write_cmd_pos)
            {
            case 0:
                nfc_write_start_page = b;
                nfc_write_cmd_pos = 1;
                break;
            case 1:
                nfc_write_expected = b;
                nfc_write_cmd_pos = 2;
                break;
            case 2:
                nfc_write_expected |= (size_t)b << 8;
                nfc_write_len = 0;
                nfc_write_cmd_pos = 3;
                if (nfc_write_expected == 0)
                {
                    nfc_write_accumulating = false;
                    nfc_write_armed = true;
                }
                break;
            default:
                if (nfc_write_len < NFC_TAG_BUF_SIZE)
                    nfc_write_buf[nfc_write_len] = b;
                nfc_write_len++;
                if (nfc_write_len >= nfc_write_expected)
                {
                    nfc_write_accumulating = false;
                    if (nfc_write_expected <= NFC_TAG_BUF_SIZE)
                        nfc_write_armed = true;
                }
                break;
            }
            continue;
        }

        switch (b)
        {
        case NFC_CMD_READ:
            nfc_read_armed = true;
            break;
        case NFC_CMD_WRITE:
            nfc_write_accumulating = true;
            nfc_write_cmd_pos = 0;
            nfc_write_armed = false;
            break;
        case NFC_CMD_CANCEL:
            nfc_write_armed = false;
            nfc_write_accumulating = false;
            break;
        case NFC_CMD_SUCCESS1:
            bel_add(&bel_nfc_success_1);
            break;
        case NFC_CMD_SUCCESS2:
            bel_add(&bel_nfc_success_2);
            break;
        case NFC_CMD_ERROR:
            bel_add(&bel_nfc_fail);
            break;
        }
    }

    *bytes_written = count;
    return STD_OK;
}

std_rw_result nfc_std_read(int desc, char *buf, uint32_t count,
                           uint32_t *bytes_read, api_errno *err)
{
    (void)desc;
    (void)err;
    uint32_t pos = 0;

    // Build read payload on first call after CMD_READ
    if (nfc_read_armed && nfc_read_len == 0)
    {
        nfc_read_armed = false;
        if (nfc_tag_ready)
        {
            size_t tag_len = nfc_tag_len;
            nfc_read_buf[0] = (uint8_t)(tag_len & 0xFF);
            nfc_read_buf[1] = (uint8_t)((tag_len >> 8) & 0xFF);
            memcpy(&nfc_read_buf[2], nfc_tag_buf, tag_len);
            nfc_read_len = 2 + tag_len;
        }
        else
        {
            memset(nfc_read_buf, 0, 2);
            nfc_read_len = 2;
        }
        nfc_read_pos = 0;
        if (count > 0)
            buf[pos++] = (char)NFC_RESP_READ;
    }

    // Drain payload (covers both continuation and freshly-built response)
    if (nfc_read_len > 0)
    {
        while (pos < count && nfc_read_pos < nfc_read_len)
            buf[pos++] = (char)nfc_read_buf[nfc_read_pos++];
        if (nfc_read_pos >= nfc_read_len)
        {
            nfc_read_pos = 0;
            nfc_read_len = 0;
        }
        *bytes_read = pos;
        return STD_OK;
    }

    // Write complete
    if (nfc_write_response)
    {
        nfc_write_response = false;
        if (count > 0)
            buf[pos++] = (char)NFC_RESP_WRITE;
        *bytes_read = pos;
        return STD_OK;
    }

    // Floor state: emit once on change (including after open)
    uint8_t floor;
    if (nfc_state == NFC_OFF || nfc_desc < 0)
        floor = NFC_RESP_NO_READER;
    else if (nfc_card_inserted && nfc_tag_ready)
        floor = NFC_RESP_CARD_READY;
    else if (nfc_card_inserted)
        floor = NFC_RESP_CARD_INSERTED;
    else
        floor = NFC_RESP_NO_CARD;
    if (floor != nfc_floor_sent)
    {
        nfc_floor_sent = floor;
        if (count > 0)
            buf[pos++] = (char)floor;
    }

    *bytes_read = pos;
    return STD_OK;
}
