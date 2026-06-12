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
};

typedef struct
{
    bool mounted;
    bool opened;
    uint8_t daddr;
    uint8_t rx_cables;
    uint8_t tx_cables;
    int8_t open_cable; // -1 merges RX cables, TX uses cable 0
    char name[MID_NAME_SIZE];
    uint32_t tick_ns;
    uint64_t epoch_ns;
    // RX
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
    if (t_ns < conn->epoch_ns)
        t_ns = conn->epoch_ns;
    uint64_t total = (t_ns - conn->epoch_ns) / conn->tick_ns;
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

static void mid_rx_pull(uint8_t idx)
{
    mid_t *conn = &mid_mounts[idx];
    while (mid_ring_free(conn->rx_head, conn->rx_tail) >= 96)
    {
        // stream_read can write bufsize+2, claim 62 into 64
        uint8_t stage[64];
        uint8_t cable;
        uint32_t n = tuh_midi_stream_read(idx, &cable, stage, sizeof(stage) - 2);
        if (!n)
            break;
        if (conn->open_cable >= 0 && cable != conn->open_cable)
            continue;
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
            conn->tx_due_ns = conn->epoch_ns + conn->tx_ticks * conn->tick_ns;
            if (conn->tx_idle)
            {
                conn->tx_idle = false;
                // Producer underrun: slip the timeline forward, never back
                if (conn->tx_due_ns < now_ns)
                {
                    conn->epoch_ns += now_ns - conn->tx_due_ns;
                    conn->tx_due_ns = now_ns;
                }
            }
            conn->tx_state = MID_TX_MSG;
            conn->tx_msg_len = 0;
            continue;
        }
        if (conn->tx_msg_len && b >= 0x80)
            conn->tx_msg_len = 0; // malformed, drop the partial message
        if (!conn->tx_msg_len)
        {
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

static size_t mid_tx_send(uint8_t idx, uint8_t cable, const uint8_t *data, size_t len)
{
    uint32_t n = tuh_midi_stream_write(idx, cable, data, len);
    if (n)
        tuh_midi_write_flush(idx);
    return n;
}

static void mid_tx_run(uint8_t idx, uint64_t now_ns)
{
    mid_t *conn = &mid_mounts[idx];
    uint8_t cable = conn->open_cable < 0 ? 0 : conn->open_cable;
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
                    if (mid_tx_send(idx, cable, &eox, 1) != 1)
                        return;
                    conn->tx_state = MID_TX_MSG;
                    conn->tx_msg_len = 0;
                    break;
                }
                if (mid_tx_send(idx, cable, &b, 1) != 1)
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
        size_t n = mid_tx_send(idx, cable, conn->tx_msg + conn->tx_msg_sent,
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
    uint8_t idx = (uint8_t)xfer->user_data;
    if (idx >= CFG_TUH_MIDI || xfer->result != XFER_RESULT_SUCCESS)
        return;
    char name[MID_NAME_SIZE];
    mid_name_to_oem((const tusb_desc_string_t *)mid_name_buf, name, sizeof(name));
    if (name[0])
        memcpy(mid_mounts[idx].name, name, sizeof(name));
}

void mid_task(void)
{
    uint64_t now_ns = time_us_64() * 1000;
    for (uint8_t idx = 0; idx < CFG_TUH_MIDI; idx++)
        if (mid_mounts[idx].mounted && mid_mounts[idx].opened)
        {
            mid_rx_pull(idx);
            mid_tx_run(idx, now_ns);
        }
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

    // Numbers 0-15 select a cable, 16+ sets microseconds per tick
    int cable = -1;
    uint32_t tick_ns = MID_DEFAULT_TICK_NS;
    const char *params = &name[6];
    while (*params)
    {
        if (!isdigit((unsigned char)*params))
        {
            *err = API_EINVAL;
            return -1;
        }
        uint32_t whole = 0;
        while (isdigit((unsigned char)*params))
        {
            whole = whole * 10 + (*params - '0');
            if (whole > 4294967) // keeps nanoseconds in 32 bits
            {
                *err = API_EINVAL;
                return -1;
            }
            params++;
        }
        bool fractional = false;
        uint32_t frac = 0;
        int frac_digits = 0;
        if (*params == '.')
        {
            params++;
            if (!isdigit((unsigned char)*params))
            {
                *err = API_EINVAL;
                return -1;
            }
            fractional = true;
            while (isdigit((unsigned char)*params))
            {
                if (frac_digits < 6)
                {
                    frac = frac * 10 + (*params - '0');
                    frac_digits++;
                }
                params++;
            }
        }
        if (!fractional && whole <= 15)
            cable = whole;
        else if (whole >= 16)
        {
            static const uint32_t pow10[7] = {1, 10, 100, 1000, 10000, 100000, 1000000};
            uint32_t frac_ns = frac_digits
                                   ? (uint32_t)(((uint64_t)frac * 1000 + pow10[frac_digits] / 2) /
                                                pow10[frac_digits])
                                   : 0;
            uint64_t ns = (uint64_t)whole * 1000 + frac_ns;
            if (ns > UINT32_MAX)
            {
                *err = API_EINVAL;
                return -1;
            }
            tick_ns = ns;
        }
        else
        {
            *err = API_EINVAL;
            return -1;
        }
        if (*params == ',')
        {
            params++;
            if (!*params)
            {
                *err = API_EINVAL;
                return -1;
            }
        }
        else if (*params)
        {
            *err = API_EINVAL;
            return -1;
        }
    }
    if (cable >= 0)
    {
        uint8_t cables = conn->rx_cables > conn->tx_cables ? conn->rx_cables
                                                           : conn->tx_cables;
        if (cable >= cables)
        {
            *err = API_EINVAL;
            return -1;
        }
    }

    // Discard anything received while closed
    for (int i = 64; i; i--)
    {
        uint8_t stage[64];
        uint8_t cable_num;
        if (!tuh_midi_stream_read(idx, &cable_num, stage, sizeof(stage) - 2))
            break;
    }

    conn->open_cable = cable;
    conn->tick_ns = tick_ns;
    conn->epoch_ns = time_us_64() * 1000;
    conn->rx_head = conn->rx_tail = 0;
    conn->rx_ticks = 0;
    conn->rx_status = 0;
    conn->rx_msg_len = 0;
    conn->rx_in_sysex = false;
    conn->rx_ring_sysex = false;
    conn->tx_head = conn->tx_tail = 0;
    conn->tx_ticks = 0;
    conn->tx_delta = 0;
    conn->tx_state = MID_TX_DELTA;
    conn->tx_msg_len = 0;
    conn->tx_status = 0;
    conn->tx_pending = false;
    conn->tx_idle = true;
    conn->opened = true;
    DBG("MIDI%d: open cable %d tick %luns\n", idx, cable, (unsigned long)tick_ns);
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
    if (!conn->mounted || !conn->opened)
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
    if (!conn->mounted || !conn->opened)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    uint32_t n = 0;
    if (conn->tx_cables)
        while (n < buf_size && mid_ring_free(conn->tx_head, conn->tx_tail))
        {
            conn->tx_ring[conn->tx_head] = buf[n++];
            conn->tx_head = (conn->tx_head + 1) & (MID_RING_SIZE - 1);
        }
    *bytes_written = n;
    return STD_OK;
}

void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *mount_cb_data)
{
    if (idx >= CFG_TUH_MIDI)
        return;
    mid_t *conn = &mid_mounts[idx];
    memset(conn, 0, sizeof(*conn));
    conn->daddr = mount_cb_data->daddr;
    conn->rx_cables = mount_cb_data->rx_cable_count;
    conn->tx_cables = mount_cb_data->tx_cable_count;
    uint16_t vid, pid;
    tuh_vid_pid_get(conn->daddr, &vid, &pid);
    snprintf(conn->name, sizeof(conn->name), "%04X:%04X", vid, pid);
    conn->mounted = true;
    DBG("MIDI%d: mount %04X:%04X dev_addr=%d\n", idx, vid, pid, conn->daddr);
    tuh_descriptor_get_product_string(conn->daddr, 0x0409,
                                      mid_name_buf, sizeof(mid_name_buf),
                                      mid_name_cb, idx);
}

void tuh_midi_umount_cb(uint8_t idx)
{
    DBG("MIDI%d: unmount\n", idx);
    if (idx < CFG_TUH_MIDI)
    {
        mid_mounts[idx].mounted = false;
        mid_mounts[idx].opened = false;
    }
}
