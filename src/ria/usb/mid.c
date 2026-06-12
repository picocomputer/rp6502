/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "usb/mid.h"
#include "api/oem.h"
#include "fatfs/ff.h"
#include "str/str.h"
#include <tusb.h>
#include <pico/time.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_MID)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// The std pipe carries SMF-style events both directions: a variable length
// quantity delta time in ticks, then a raw wire MIDI message. Writes are
// released to the device on schedule, reads are a timestamped recording.

__in_flash("mid_string") static const char mid_string[] = "MIDI";
static_assert(sizeof(mid_string) == 4 + 1);

#define MID_RING_SIZE 128
#define MID_NAME_SIZE 32
#define MID_DEFAULT_TICK_NS 1041667 // 120 BPM at 480 PPQN

enum
{
    MID_TX_DELTA,
    MID_TX_MSG,
    MID_TX_SYSEX,
    MID_TX_META_TYPE,
    MID_TX_META_LEN,
    MID_TX_META_DATA,
};

typedef struct
{
    bool mounted;
    bool opened;
    uint8_t daddr;
    uint8_t itf;   // TinyUSB interface this cable lives on
    uint8_t cable; // cable number within the interface
    bool has_rx;
    bool has_tx;
    char name[MID_NAME_SIZE];
    uint32_t tick_ns;
    uint16_t ppqn; // division for FF 51 tempo conversion, 0 = unset
    // RX
    uint64_t rx_epoch_ns;
    uint16_t rx_head, rx_tail;
    uint64_t rx_ticks;
    uint8_t rx_status;
    uint8_t rx_msg[3];
    uint8_t rx_msg_len;
    uint8_t rx_msg_need;
    bool rx_in_sysex;
    bool rx_ring_sysex;
    uint8_t rx_ring[MID_RING_SIZE];
    // TX
    uint64_t tx_epoch_ns;
    uint16_t tx_head, tx_tail;
    uint64_t tx_ticks;
    uint64_t tx_due_ns;
    uint32_t tx_delta;
    uint8_t tx_state;
    uint8_t tx_msg[3];
    uint8_t tx_msg_len;
    uint8_t tx_msg_sent;
    uint8_t tx_msg_need;
    uint8_t tx_status;
    bool tx_pending;
    bool tx_idle;
    uint8_t tx_meta_type; // FF meta consumed locally, applied at its due time
    uint32_t tx_meta_len;
    uint32_t tx_meta_rem;
    bool tx_meta;
    uint8_t tx_ring[MID_RING_SIZE];
} mid_t;
static mid_t mid_mounts[CFG_TUH_MIDI];
static_assert(CFG_TUH_MIDI <= 10); // one char 0-9 in "MIDI0:"

// Product string fetches share one buffer, serialized by the control pipe.
static uint8_t mid_name_buf[64];

static inline uint16_t mid_ring_free(uint16_t head, uint16_t tail)
{
    return (uint16_t)((MID_RING_SIZE - 1) - ((head - tail) & (MID_RING_SIZE - 1)));
}

// Data byte count for a status byte, sysex handled separately.
static uint8_t mid_msg_data_len(uint8_t status)
{
    switch (status & 0xF0)
    {
    case 0x80:
    case 0x90:
    case 0xA0:
    case 0xB0:
    case 0xE0:
        return 2;
    case 0xC0:
    case 0xD0:
        return 1;
    case 0xF0:
        switch (status)
        {
        case 0xF1:
        case 0xF3:
            return 1;
        case 0xF2:
            return 2;
        default:
            return 0;
        }
    }
    return 0;
}

static uint8_t mid_vlq_len(uint32_t v)
{
    if (v < 0x80)
        return 1;
    if (v < 0x4000)
        return 2;
    if (v < 0x200000)
        return 3;
    return 4;
}

static inline void mid_rx_push(mid_t *conn, uint8_t b)
{
    conn->rx_ring[conn->rx_head] = b;
    conn->rx_head = (conn->rx_head + 1) & (MID_RING_SIZE - 1);
}

static void mid_rx_push_vlq(mid_t *conn, uint32_t v)
{
    for (int shift = (mid_vlq_len(v) - 1) * 7; shift > 0; shift -= 7)
        mid_rx_push(conn, 0x80 | ((v >> shift) & 0x7F));
    mid_rx_push(conn, v & 0x7F);
}

static void mid_rx_push_sysex_end(mid_t *conn)
{
    if (!conn->rx_ring_sysex)
        return;
    mid_rx_push(conn, 0xF7); // space reserved while sysex is open
    conn->rx_ring_sysex = false;
}

// Push one delta-prefixed event, dropped whole when the ring is full.
// Absolute tick anchoring keeps timing exact across drops.
static void mid_rx_push_event(mid_t *conn, uint64_t t_ns, const uint8_t *msg, uint8_t len)
{
    if (conn->rx_ring_sysex)
    {
        // No deltas inside sysex, they would be ambiguous with data bytes
        if (len == 1 && msg[0] >= 0xF8)
        {
            if (mid_ring_free(conn->rx_head, conn->rx_tail) > 1)
                mid_rx_push(conn, msg[0]);
            return;
        }
        mid_rx_push_sysex_end(conn);
    }
    if (t_ns < conn->rx_epoch_ns)
        t_ns = conn->rx_epoch_ns;
    uint64_t total = (t_ns - conn->rx_epoch_ns) / conn->tick_ns;
    uint32_t delta = 0;
    if (total > conn->rx_ticks)
        delta = total - conn->rx_ticks > 0x0FFFFFFF
                    ? 0x0FFFFFFF
                    : (uint32_t)(total - conn->rx_ticks);
    uint16_t reserve = msg[0] == 0xF0 ? 1 : 0; // sysex keeps space for F7
    if (mid_ring_free(conn->rx_head, conn->rx_tail) < mid_vlq_len(delta) + len + reserve)
        return;
    conn->rx_ticks += delta;
    mid_rx_push_vlq(conn, delta);
    for (uint8_t i = 0; i < len; i++)
        mid_rx_push(conn, msg[i]);
    if (msg[0] == 0xF0)
        conn->rx_ring_sysex = true;
}

static void mid_rx_push_sysex_data(mid_t *conn, uint8_t b)
{
    if (!conn->rx_ring_sysex)
        return;
    if (mid_ring_free(conn->rx_head, conn->rx_tail) > 1) // keep F7 space
        mid_rx_push(conn, b);
}

static void mid_rx_wire_byte(mid_t *conn, uint64_t t_ns, uint8_t b)
{
    if (b >= 0xF8)
    {
        mid_rx_push_event(conn, t_ns, &b, 1);
        return;
    }
    if (b == 0xF7)
    {
        if (conn->rx_in_sysex)
        {
            conn->rx_in_sysex = false;
            mid_rx_push_sysex_end(conn);
        }
        return;
    }
    if (b == 0xF0)
    {
        conn->rx_in_sysex = true;
        conn->rx_status = 0;
        conn->rx_msg_len = 0;
        mid_rx_push_event(conn, t_ns, &b, 1);
        return;
    }
    if (b >= 0x80)
    {
        if (conn->rx_in_sysex)
        {
            conn->rx_in_sysex = false;
            mid_rx_push_sysex_end(conn);
        }
        if (b >= 0xF1)
        {
            conn->rx_status = 0;
            if (b == 0xF4 || b == 0xF5)
            {
                conn->rx_msg_len = 0;
                return;
            }
        }
        else
            conn->rx_status = b;
        conn->rx_msg[0] = b;
        conn->rx_msg_len = 1;
        conn->rx_msg_need = mid_msg_data_len(b);
        if (!conn->rx_msg_need)
        {
            mid_rx_push_event(conn, t_ns, conn->rx_msg, 1);
            conn->rx_msg_len = 0;
        }
        return;
    }
    if (conn->rx_in_sysex)
    {
        mid_rx_push_sysex_data(conn, b);
        return;
    }
    if (!conn->rx_msg_len)
    {
        if (!conn->rx_status)
            return;
        conn->rx_msg[0] = conn->rx_status;
        conn->rx_msg_len = 1;
        conn->rx_msg_need = mid_msg_data_len(conn->rx_status);
    }
    conn->rx_msg[conn->rx_msg_len++] = b;
    if (!--conn->rx_msg_need)
    {
        mid_rx_push_event(conn, t_ns, conn->rx_msg, conn->rx_msg_len);
        conn->rx_msg_len = 0;
    }
}

static mid_t *mid_find_port(uint8_t itf, uint8_t cable)
{
    for (uint8_t i = 0; i < CFG_TUH_MIDI; i++)
        if (mid_mounts[i].mounted && mid_mounts[i].itf == itf &&
            mid_mounts[i].cable == cable)
            return &mid_mounts[i];
    return NULL;
}

// True while some open input cable on the interface has room to receive.
static bool mid_itf_can_rx(uint8_t itf)
{
    for (uint8_t i = 0; i < CFG_TUH_MIDI; i++)
    {
        mid_t *p = &mid_mounts[i];
        if (p->mounted && p->opened && p->has_rx && p->itf == itf &&
            mid_ring_free(p->rx_head, p->rx_tail) >= 96)
            return true;
    }
    return false;
}

// One interface FIFO carries every cable; demux each chunk to its port.
static void mid_rx_pull_itf(uint8_t itf)
{
    while (mid_itf_can_rx(itf))
    {
        // stream_read can write bufsize+2, claim 62 into 64
        uint8_t stage[64];
        uint8_t cable;
        uint32_t n = tuh_midi_stream_read(itf, &cable, stage, sizeof(stage) - 2);
        if (!n)
            break;
        mid_t *conn = mid_find_port(itf, cable);
        if (!conn || !conn->opened || !conn->has_rx)
            continue; // closed or absent cable, drop
        uint64_t t_ns = time_us_64() * 1000;
        for (uint32_t i = 0; i < n; i++)
            mid_rx_wire_byte(conn, t_ns, stage[i]);
    }
}

// Parse the next delta-prefixed event from the TX ring. True when tx_msg
// holds a complete unsent message with tx_due_ns computed.
static bool mid_tx_parse(mid_t *conn, uint64_t now_ns)
{
    while (!conn->tx_pending && conn->tx_state != MID_TX_SYSEX &&
           conn->tx_head != conn->tx_tail)
    {
        uint8_t b = conn->tx_ring[conn->tx_tail];
        conn->tx_tail = (conn->tx_tail + 1) & (MID_RING_SIZE - 1);
        if (conn->tx_state == MID_TX_DELTA)
        {
            conn->tx_delta = (conn->tx_delta << 7) | (b & 0x7F);
            if (b & 0x80)
                continue;
            conn->tx_ticks += conn->tx_delta;
            conn->tx_delta = 0;
            conn->tx_due_ns = conn->tx_epoch_ns + conn->tx_ticks * conn->tick_ns;
            if (conn->tx_idle)
            {
                conn->tx_idle = false;
                // Producer underrun: slip the timeline forward, never back
                if (conn->tx_due_ns < now_ns)
                {
                    conn->tx_epoch_ns += now_ns - conn->tx_due_ns;
                    conn->tx_due_ns = now_ns;
                }
            }
            conn->tx_state = MID_TX_MSG;
            conn->tx_msg_len = 0;
            continue;
        }
        if (conn->tx_state == MID_TX_META_TYPE)
        {
            conn->tx_meta_type = b;
            conn->tx_meta_len = 0;
            conn->tx_state = MID_TX_META_LEN;
            continue;
        }
        if (conn->tx_state == MID_TX_META_LEN)
        {
            conn->tx_meta_len = (conn->tx_meta_len << 7) | (b & 0x7F);
            if (b & 0x80)
                continue;
            conn->tx_meta_rem = conn->tx_meta_len;
            conn->tx_msg_len = 0;
            if (conn->tx_meta_rem)
                conn->tx_state = MID_TX_META_DATA;
            else
                conn->tx_pending = conn->tx_meta = true;
            continue;
        }
        if (conn->tx_state == MID_TX_META_DATA)
        {
            if (conn->tx_msg_len < sizeof(conn->tx_msg))
                conn->tx_msg[conn->tx_msg_len++] = b;
            if (!--conn->tx_meta_rem)
                conn->tx_pending = conn->tx_meta = true;
            continue;
        }
        if (conn->tx_msg_len && b >= 0x80)
            conn->tx_msg_len = 0; // malformed, drop the partial message
        if (!conn->tx_msg_len)
        {
            if (b == 0xFF) // meta event: consumed locally, never sent
            {
                conn->tx_state = MID_TX_META_TYPE;
                continue;
            }
            if (b == 0xF4 || b == 0xF5)
            {
                conn->tx_state = MID_TX_DELTA;
                continue;
            }
            if (b < 0x80)
            {
                if (!conn->tx_status)
                {
                    conn->tx_state = MID_TX_DELTA;
                    continue;
                }
                conn->tx_msg[0] = conn->tx_status;
                conn->tx_msg[1] = b;
                conn->tx_msg_len = 2;
                conn->tx_msg_need = mid_msg_data_len(conn->tx_status) - 1;
            }
            else
            {
                if (b == 0xF0 || (b >= 0xF1 && b <= 0xF6))
                    conn->tx_status = 0;
                else if (b < 0xF0)
                    conn->tx_status = b;
                conn->tx_msg[0] = b;
                conn->tx_msg_len = 1;
                conn->tx_msg_need = mid_msg_data_len(b);
            }
        }
        else
        {
            conn->tx_msg[conn->tx_msg_len++] = b;
            conn->tx_msg_need--;
        }
        if (conn->tx_msg_len && !conn->tx_msg_need)
        {
            conn->tx_msg_sent = 0;
            conn->tx_pending = true;
        }
    }
    return conn->tx_pending;
}

static size_t mid_tx_send(uint8_t itf, uint8_t cable, const uint8_t *data, size_t len)
{
    uint32_t n = tuh_midi_stream_write(itf, cable, data, len);
    if (n)
        tuh_midi_write_flush(itf);
    return n;
}

// Apply a TX-stream meta locally (never sent to the device) and mark the
// effect on the RX recording: the resolved change, or FF 60 on rejection.
static void mid_tx_apply_meta(mid_t *conn)
{
    uint64_t t_ns = conn->tx_due_ns;
    switch (conn->tx_meta_type)
    {
    case 0x51: // SMF Set Tempo: microseconds per quarter note
        if (conn->tx_meta_len == 3 && conn->ppqn)
        {
            uint32_t tempo = ((uint32_t)conn->tx_msg[0] << 16) |
                             ((uint32_t)conn->tx_msg[1] << 8) | conn->tx_msg[2];
            uint64_t tick = ((uint64_t)tempo * 1000 + conn->ppqn / 2) / conn->ppqn;
            if (tempo && tick && tick <= UINT32_MAX)
            {
                mid_rx_push_event(conn, t_ns, (const uint8_t[]){0xFF, 0x51, 0x03,
                                  conn->tx_msg[0], conn->tx_msg[1], conn->tx_msg[2]}, 6);
                conn->tick_ns = (uint32_t)tick;
                conn->rx_epoch_ns = conn->tx_epoch_ns = t_ns;
                conn->rx_ticks = conn->tx_ticks = 0;
                return;
            }
        }
        break;
    case 0x61: // Set PPQN division for tempo conversion
        if (conn->tx_meta_len == 2 && (conn->tx_msg[0] || conn->tx_msg[1]))
        {
            conn->ppqn = ((uint16_t)conn->tx_msg[0] << 8) | conn->tx_msg[1];
            return; // no audible effect until the next tempo
        }
        break;
    default:
        return; // unknown meta (standard SMF metas included): swallowed
    }
    mid_rx_push_event(conn, t_ns, (const uint8_t[]){0xFF, 0x60, 0x01, conn->tx_meta_type}, 4);
}

static void mid_tx_run(uint8_t idx, uint64_t now_ns)
{
    mid_t *conn = &mid_mounts[idx];
    for (;;)
    {
        if (conn->tx_state == MID_TX_SYSEX)
        {
            while (conn->tx_head != conn->tx_tail)
            {
                uint8_t b = conn->tx_ring[conn->tx_tail];
                if (b >= 0x80 && b < 0xF7)
                {
                    // New status mid-sysex, terminate and start next event
                    uint8_t eox = 0xF7;
                    if (mid_tx_send(conn->itf, conn->cable, &eox, 1) != 1)
                        return;
                    conn->tx_state = MID_TX_MSG;
                    conn->tx_msg_len = 0;
                    break;
                }
                if (mid_tx_send(conn->itf, conn->cable, &b, 1) != 1)
                    return;
                conn->tx_tail = (conn->tx_tail + 1) & (MID_RING_SIZE - 1);
                if (b == 0xF7)
                {
                    conn->tx_state = MID_TX_DELTA;
                    break;
                }
            }
            if (conn->tx_state == MID_TX_SYSEX)
                return;
            continue;
        }
        if (!mid_tx_parse(conn, now_ns))
        {
            conn->tx_idle = true;
            return;
        }
        if (conn->tx_due_ns > now_ns)
            return;
        if (conn->tx_meta)
        {
            mid_tx_apply_meta(conn);
            conn->tx_pending = conn->tx_meta = false;
            conn->tx_state = MID_TX_DELTA;
            continue;
        }
        size_t n = mid_tx_send(conn->itf, conn->cable, conn->tx_msg + conn->tx_msg_sent,
                               conn->tx_msg_len - conn->tx_msg_sent);
        conn->tx_msg_sent += n;
        if (conn->tx_msg_sent < conn->tx_msg_len)
            return;
        conn->tx_pending = false;
        conn->tx_state = conn->tx_msg[0] == 0xF0 ? MID_TX_SYSEX : MID_TX_DELTA;
    }
}

// Convert USB string descriptor to OEM for display.
static void mid_name_to_oem(const tusb_desc_string_t *desc, char *dest, size_t dest_size)
{
    uint16_t ulen = 0;
    if (desc->bDescriptorType == TUSB_DESC_STRING && desc->bLength >= 2)
        ulen = (desc->bLength - 2) / 2;
    uint16_t max_ulen = (sizeof(mid_name_buf) - sizeof(tusb_desc_string_t)) / sizeof(uint16_t);
    if (ulen > max_ulen)
        ulen = max_ulen;
    uint16_t cp = oem_get_code_page_run();
    size_t pos = 0;
    for (uint16_t i = 0; i < ulen && pos < dest_size - 1; i++)
    {
        WCHAR ch = ff_uni2oem(desc->utf16le[i], cp);
        dest[pos++] = ch ? (char)ch : '?';
    }
    dest[pos] = '\0';
}

static void mid_name_cb(tuh_xfer_t *xfer)
{
    uint8_t itf = (uint8_t)xfer->user_data;
    if (xfer->result != XFER_RESULT_SUCCESS)
        return;
    char name[MID_NAME_SIZE];
    mid_name_to_oem((const tusb_desc_string_t *)mid_name_buf, name, sizeof(name));
    if (!name[0])
        return;
    for (uint8_t i = 0; i < CFG_TUH_MIDI; i++)
        if (mid_mounts[i].mounted && mid_mounts[i].itf == itf)
            memcpy(mid_mounts[i].name, name, sizeof(name));
}

void mid_task(void)
{
    for (uint8_t itf = 0; itf < CFG_TUH_MIDI; itf++)
        mid_rx_pull_itf(itf);
    uint64_t now_ns = time_us_64() * 1000;
    for (uint8_t idx = 0; idx < CFG_TUH_MIDI; idx++)
        if (mid_mounts[idx].mounted && mid_mounts[idx].opened)
            mid_tx_run(idx, now_ns);
}

int mid_status_count(void)
{
    int count = 0;
    for (uint8_t idx = 0; idx < CFG_TUH_MIDI; idx++)
        if (mid_mounts[idx].mounted)
            count++;
    return count;
}

int mid_status_response(char *buf, size_t buf_size, int state)
{
    if (state < 0 || state >= CFG_TUH_MIDI)
        return -1;
    mid_t *conn = &mid_mounts[state];
    if (conn->mounted)
    {
        char devname[sizeof(mid_string) + 2];
        snprintf(devname, sizeof(devname), "%s%d", mid_string, state);
        snprintf_utf8(buf, buf_size, STR_STATUS_MIDI, devname, conn->name);
    }
    return state + 1;
}

bool mid_std_handles(const char *name)
{
    if (strncasecmp(name, mid_string, sizeof(mid_string) - 1) != 0)
        return false;
    if (!isdigit((unsigned char)name[4]))
        return false;
    if (name[5] != ':')
        return false;
    return true;
}

int mid_std_open(const char *name, uint8_t flags, api_errno *err)
{
    (void)flags;
    if (!mid_std_handles(name))
    {
        *err = API_ENOENT;
        return -1;
    }
    uint8_t idx = name[4] - '0';
    if (idx >= CFG_TUH_MIDI || !mid_mounts[idx].mounted)
    {
        *err = API_ENODEV;
        return -1;
    }
    mid_t *conn = &mid_mounts[idx];
    if (conn->opened)
    {
        *err = API_EBUSY;
        return -1;
    }

    if (name[6]) // cable and tempo are set with in-stream meta events now
    {
        *err = API_EINVAL;
        return -1;
    }

    // Stale RX accumulates only while the whole interface is idle; if no
    // sibling cable is open, flush it so this cable starts clean.
    bool itf_idle = true;
    for (uint8_t i = 0; i < CFG_TUH_MIDI; i++)
        if (i != idx && mid_mounts[i].mounted && mid_mounts[i].opened &&
            mid_mounts[i].itf == conn->itf)
            itf_idle = false;
    if (itf_idle)
        for (int i = 64; i; i--)
        {
            uint8_t stage[64];
            uint8_t cable_num;
            if (!tuh_midi_stream_read(conn->itf, &cable_num, stage, sizeof(stage) - 2))
                break;
        }

    uint64_t now_ns = time_us_64() * 1000;
    conn->tick_ns = MID_DEFAULT_TICK_NS;
    conn->ppqn = 0;
    conn->rx_epoch_ns = now_ns;
    conn->rx_head = conn->rx_tail = 0;
    conn->rx_ticks = 0;
    conn->rx_status = 0;
    conn->rx_msg_len = 0;
    conn->rx_in_sysex = false;
    conn->rx_ring_sysex = false;
    conn->tx_epoch_ns = now_ns;
    conn->tx_head = conn->tx_tail = 0;
    conn->tx_ticks = 0;
    conn->tx_delta = 0;
    conn->tx_state = MID_TX_DELTA;
    conn->tx_msg_len = 0;
    conn->tx_status = 0;
    conn->tx_pending = false;
    conn->tx_idle = true;
    conn->tx_meta = false;
    conn->opened = true;
    DBG("MIDI%d: open\n", idx);
    return idx;
}

int mid_std_close(int desc, api_errno *err)
{
    if (!mid_mounts[desc].opened)
    {
        *err = API_EBADF;
        return -1;
    }
    DBG("MIDI%d: close\n", desc);
    mid_mounts[desc].opened = false;
    return 0;
}

std_rw_result mid_std_read(int desc, char *buf, uint32_t buf_size,
                           uint32_t *bytes_read, api_errno *err)
{
    mid_t *conn = &mid_mounts[desc];
    if (!conn->mounted || !conn->opened || !conn->has_rx)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    uint32_t n = 0;
    while (n < buf_size && conn->rx_tail != conn->rx_head)
    {
        buf[n++] = conn->rx_ring[conn->rx_tail];
        conn->rx_tail = (conn->rx_tail + 1) & (MID_RING_SIZE - 1);
    }
    *bytes_read = n;
    return STD_OK;
}

std_rw_result mid_std_write(int desc, const char *buf, uint32_t buf_size,
                            uint32_t *bytes_written, api_errno *err)
{
    mid_t *conn = &mid_mounts[desc];
    if (!conn->mounted || !conn->opened || !conn->has_tx)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    uint32_t n = 0;
    while (n < buf_size && mid_ring_free(conn->tx_head, conn->tx_tail))
    {
        conn->tx_ring[conn->tx_head] = buf[n++];
        conn->tx_head = (conn->tx_head + 1) & (MID_RING_SIZE - 1);
    }
    *bytes_written = n;
    return STD_OK;
}

void tuh_midi_mount_cb(uint8_t itf, const tuh_midi_mount_cb_t *mount_cb_data)
{
    if (itf >= CFG_TUH_MIDI)
        return;
    uint8_t rx = mount_cb_data->rx_cable_count;
    uint8_t tx = mount_cb_data->tx_cable_count;
    uint8_t ncables = rx > tx ? rx : tx;
    uint16_t vid, pid;
    tuh_vid_pid_get(mount_cb_data->daddr, &vid, &pid);
    for (uint8_t c = 0; c < ncables; c++)
    {
        uint8_t slot = CFG_TUH_MIDI;
        for (uint8_t i = 0; i < CFG_TUH_MIDI; i++)
            if (!mid_mounts[i].mounted)
            {
                slot = i;
                break;
            }
        if (slot == CFG_TUH_MIDI)
            break; // no free port; remaining cables are dropped
        mid_t *conn = &mid_mounts[slot];
        memset(conn, 0, sizeof(*conn));
        conn->daddr = mount_cb_data->daddr;
        conn->itf = itf;
        conn->cable = c;
        conn->has_rx = c < rx;
        conn->has_tx = c < tx;
        snprintf(conn->name, sizeof(conn->name), "%04X:%04X", vid, pid);
        conn->mounted = true;
        DBG("MIDI%u: itf %u cable %u %c%c %04X:%04X\n", slot, itf, c,
            conn->has_rx ? 'R' : '-', conn->has_tx ? 'T' : '-', vid, pid);
    }
    tuh_descriptor_get_product_string(mount_cb_data->daddr, 0x0409,
                                      mid_name_buf, sizeof(mid_name_buf),
                                      mid_name_cb, itf);
}

void tuh_midi_umount_cb(uint8_t itf)
{
    DBG("MIDI: unmount itf %u\n", itf);
    for (uint8_t i = 0; i < CFG_TUH_MIDI; i++)
        if (mid_mounts[i].mounted && mid_mounts[i].itf == itf)
        {
            mid_mounts[i].mounted = false;
            mid_mounts[i].opened = false;
        }
}
