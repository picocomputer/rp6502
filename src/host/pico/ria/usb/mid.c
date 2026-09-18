/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/usb/mid.h"
#include "core/str/str.h"
#include "ria/usb/usb.h"
#include "core/sys/debug_log.h"
#include <tusb.h>
#include <pico/time.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

extern uint8_t tuh_midi_frame(uint8_t idx, const uint8_t *pkt, uint8_t *out);

__in_flash("mid_string") static const char mid_string[] = "MIDI";
static_assert(sizeof(mid_string) == 4 + 1);

#define MID_RING_SIZE 128
#define MID_DEFAULT_TEMPO 500000 // microseconds per quarter note, 120 BPM
#define MID_MAX_PPQN 32767       // SMF division with bit 15 clear

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
    uint8_t session;
    uint8_t daddr;
    uint8_t itf;
    uint8_t cable;
    bool has_rx;
    bool has_tx;
    uint32_t tick_ns;
    uint32_t tempo;
    uint16_t ppqn;
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
    uint64_t tx_epoch_ns;
    uint16_t tx_head, tx_tail;
    uint64_t tx_ticks;
    uint64_t tx_due_ns;
    uint32_t tx_delta;
    uint8_t tx_state;
    uint8_t tx_msg[3];
    uint8_t tx_msg_len;
    uint8_t tx_msg_need;
    uint8_t tx_status;
    bool tx_pending;
    bool tx_idle;
    uint8_t tx_rt;
    uint8_t tx_acc[3];
    uint8_t tx_acc_len;
    bool tx_acc_eox;
    bool tx_sysex_break;
    uint8_t tx_meta_type;
    uint32_t tx_meta_len;
    uint32_t tx_meta_rem;
    bool tx_meta;
    uint8_t tx_ring[MID_RING_SIZE];
} mid_t;
static mid_t mid_mounts[CFG_TUH_MIDI];
static_assert(CFG_TUH_MIDI <= 10); // one char 0-9 in "MIDI0:"

static inline uint16_t mid_ring_free(uint16_t head, uint16_t tail)
{
    return (uint16_t)((MID_RING_SIZE - 1) - ((head - tail) & (MID_RING_SIZE - 1)));
}

static inline bool mid_raw(const mid_t *conn)
{
    return !conn->ppqn;
}

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

static uint8_t mid_msg_cin(uint8_t status)
{
    if (status >= 0xF8)
        return MIDI_CIN_1BYTE_DATA;
    if (status < 0xF0)
        return status >> 4;
    switch (status)
    {
    case 0xF1:
    case 0xF3:
        return MIDI_CIN_SYSCOM_2BYTE;
    case 0xF2:
        return MIDI_CIN_SYSCOM_3BYTE;
    default:
        return MIDI_CIN_SYSEX_END_1BYTE;
    }
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
    mid_rx_push(conn, 0xF7); // one ring byte is kept free for this F7
    conn->rx_ring_sysex = false;
}

// An event that does not fit is dropped whole. The next event's delta then
// includes the time of the dropped one, because each delta is the absolute
// tick count since rx_epoch_ns less rx_ticks, and rx_ticks advances only when
// an event is pushed. The exception is a dropped echo of an applied Set Tempo
// meta, because mid_tx_apply_meta sets rx_epoch_ns to the echo's time and
// rx_ticks to 0 even when the echo is dropped, so the next delta counts only
// from that time.
static bool mid_rx_push_event(mid_t *conn, uint64_t t_ns, const uint8_t *msg, uint8_t len)
{
    // A realtime byte inside an open recorded sysex gets no delta, because a
    // delta byte there could not be distinguished from sysex data.
    if (conn->rx_ring_sysex && len == 1 && msg[0] >= 0xF8)
    {
        if (mid_ring_free(conn->rx_head, conn->rx_tail) <= 1)
            return false;
        mid_rx_push(conn, msg[0]);
        return true;
    }
    if (t_ns < conn->rx_epoch_ns)
        t_ns = conn->rx_epoch_ns;
    uint64_t total = (t_ns - conn->rx_epoch_ns) / conn->tick_ns;
    uint32_t delta = 0;
    if (total > conn->rx_ticks)
        delta = total - conn->rx_ticks > 0x0FFFFFFF
                    ? 0x0FFFFFFF
                    : (uint32_t)(total - conn->rx_ticks);
    uint16_t reserve = msg[0] == 0xF0 ? 1 : 0;
    uint16_t close = conn->rx_ring_sysex ? 1 : 0;
    if (mid_ring_free(conn->rx_head, conn->rx_tail) < mid_vlq_len(delta) + len + reserve + close)
        return false;
    if (conn->rx_ring_sysex)
        mid_rx_push_sysex_end(conn);
    conn->rx_ticks += delta;
    mid_rx_push_vlq(conn, delta);
    for (uint8_t i = 0; i < len; i++)
        mid_rx_push(conn, msg[i]);
    if (msg[0] == 0xF0)
        conn->rx_ring_sysex = true;
    return true;
}

static void mid_rx_push_sysex_data(mid_t *conn, uint8_t b)
{
    if (!conn->rx_ring_sysex)
        return;
    if (mid_ring_free(conn->rx_head, conn->rx_tail) > 1)
        mid_rx_push(conn, b);
}

static void mid_rx_wire_byte(mid_t *conn, uint64_t t_ns, uint8_t b)
{
    if (b == 0xFF) // System Reset is recorded as the escape FF FF
    {
        if (conn->rx_in_sysex)
        {
            conn->rx_in_sysex = false;
            mid_rx_push_sysex_end(conn);
        }
        conn->rx_status = 0;
        conn->rx_msg_len = 0;
        mid_rx_push_event(conn, t_ns, (const uint8_t[]){0xFF, 0xFF}, 2);
        return;
    }
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
        conn->rx_status = 0; // EOX is system common, so it cancels running status
        conn->rx_msg_len = 0;
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

static void mid_rx_sink(mid_t *conn, uint64_t t_ns, uint8_t b)
{
    if (mid_raw(conn))
    {
        if (mid_ring_free(conn->rx_head, conn->rx_tail))
            mid_rx_push(conn, b);
    }
    else
        mid_rx_wire_byte(conn, t_ns, b);
}

static void mid_rx_pull_itf(uint8_t itf)
{
    while (mid_itf_can_rx(itf))
    {
        uint8_t stage[64];
        uint32_t n = tuh_midi_packet_read_n(itf, stage, sizeof(stage));
        if (!n)
            break;
        uint64_t t_ns = time_us_64() * 1000;
        for (uint32_t i = 0; i + 4 <= n; i += 4)
        {
            mid_t *conn = mid_find_port(itf, stage[i] >> 4);
            if (!conn || !conn->opened || !conn->has_rx)
                continue;
            uint8_t wire[3];
            uint8_t m = tuh_midi_frame(itf, &stage[i], wire);
            for (uint8_t j = 0; j < m; j++)
                mid_rx_sink(conn, t_ns, wire[j]);
        }
    }
}

// Callers drop a data byte that would start a message while tx_status is 0,
// because mid_msg_data_len returns 0 for a status of 0, so tx_msg_need would
// wrap to 255 and later data bytes would be written past the end of tx_msg.
static void mid_tx_msg_byte(mid_t *conn, uint8_t b)
{
    if (!conn->tx_msg_len)
    {
        if (b < 0x80)
        {
            conn->tx_msg[0] = conn->tx_status;
            conn->tx_msg[1] = b;
            conn->tx_msg_len = 2;
            conn->tx_msg_need = mid_msg_data_len(conn->tx_status) - 1;
        }
        else
        {
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
        conn->tx_pending = true;
}

static bool mid_tx_parse(mid_t *conn, uint64_t now_ns)
{
    while (!conn->tx_pending && conn->tx_state != MID_TX_SYSEX &&
           conn->tx_head != conn->tx_tail)
    {
        uint8_t b = conn->tx_ring[conn->tx_tail];
        conn->tx_tail = (conn->tx_tail + 1) & (MID_RING_SIZE - 1);
        if (conn->tx_state == MID_TX_DELTA)
        {
            if (conn->tx_delta > (0x0FFFFFFF >> 7)) // saturate at the SMF max
                conn->tx_delta = 0x0FFFFFFF;
            else
                conn->tx_delta = (conn->tx_delta << 7) | (b & 0x7F);
            if (b & 0x80)
                continue;
            conn->tx_ticks += conn->tx_delta;
            conn->tx_delta = 0;
            conn->tx_due_ns = conn->tx_epoch_ns + conn->tx_ticks * conn->tick_ns;
            if (conn->tx_idle)
            {
                conn->tx_idle = false;
                // When the ring ran empty before this event and the event is
                // already late, tx_epoch_ns is moved forward so the event is
                // due now and the events after it keep their spacing.
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
            if (b == 0xFF) // FF FF is an escaped System Reset
            {
                conn->tx_status = 0;
                conn->tx_msg[0] = 0xFF;
                conn->tx_msg_len = 1;
                conn->tx_pending = true;
                continue;
            }
            conn->tx_meta_type = b;
            conn->tx_meta_len = 0;
            conn->tx_state = MID_TX_META_LEN;
            continue;
        }
        if (conn->tx_state == MID_TX_META_LEN)
        {
            if (conn->tx_meta_len > (0x0FFFFFFF >> 7)) // saturate at the SMF max
                conn->tx_meta_len = 0x0FFFFFFF;
            else
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
        if (b >= 0xF8 && b != 0xFF && conn->tx_msg_len)
        {
            // A realtime byte can arrive between the bytes of a message. It is
            // left in tx_rt for mid_tx_run to send next, and the partial
            // message stays in tx_msg to collect its remaining bytes.
            conn->tx_rt = b;
            break;
        }
        if (conn->tx_msg_len && b >= 0x80)
            conn->tx_msg_len = 0;
        if (!conn->tx_msg_len)
        {
            if (b == 0xFF)
            {
                conn->tx_state = MID_TX_META_TYPE;
                continue;
            }
            if (b == 0xF4 || b == 0xF5)
            {
                conn->tx_status = 0; // system common cancels running status
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
            }
            else if (b == 0xF0 || (b >= 0xF1 && b <= 0xF7))
                conn->tx_status = 0;
            else if (b < 0xF0)
                conn->tx_status = b;
        }
        mid_tx_msg_byte(conn, b);
    }
    return conn->tx_pending;
}

// The endpoint FIFO only ever holds whole packets, so a write stores 4 bytes
// or none.
static bool mid_tx_packet(mid_t *conn, uint8_t cin, const uint8_t *data, uint8_t len)
{
    uint8_t pkt[4] = {(uint8_t)((conn->cable << 4) | cin), 0, 0, 0};
    for (uint8_t i = 0; i < len; i++)
        pkt[1 + i] = data[i];
    return tuh_midi_packet_write_n(conn->itf, pkt, 4) == 4;
}

static void mid_tx_apply_meta(mid_t *conn)
{
    uint64_t t_ns = conn->tx_due_ns;
    switch (conn->tx_meta_type)
    {
    case 0x51: // SMF Set Tempo: microseconds per quarter note
    {
        uint32_t tempo = 0;
        if (conn->tx_meta_len == 3)
            tempo = ((uint32_t)conn->tx_msg[0] << 16) |
                    ((uint32_t)conn->tx_msg[1] << 8) | conn->tx_msg[2];
        uint64_t tick = tempo ? ((uint64_t)tempo * 1000 + conn->ppqn / 2) / conn->ppqn : 0;
        if (!tick || tick > UINT32_MAX)
            tempo = 0;
        // The Set Tempo meta is echoed to the RX ring, with a tempo of 0 when
        // the new tempo is rejected. The echo is pushed before the new tempo
        // is applied below, because that step sets rx_epoch_ns to the echo's
        // time and rx_ticks to 0, so an echo pushed after it would always get
        // a delta of 0.
        bool device_sysex = conn->rx_ring_sysex && conn->rx_in_sysex;
        bool pushed = mid_rx_push_event(
            conn, t_ns,
            (const uint8_t[]){0xFF, 0x51, 0x03, (uint8_t)(tempo >> 16),
                              (uint8_t)(tempo >> 8), (uint8_t)tempo},
            6);
        // Pushing the echo closes the open recorded sysex with an F7, so an F0
        // is pushed after it, when it fits, to reopen the sysex for the rest of
        // the device's data.
        if (device_sysex && pushed)
            mid_rx_push_event(conn, t_ns, (const uint8_t[]){0xF0}, 1);
        // The tempo is applied even when the echo does not fit, because echoes
        // fill the RX ring of a cable that is never read, and tempo changes on
        // that cable would otherwise stop once its ring is full.
        if (tempo)
        {
            conn->tempo = tempo;
            conn->tick_ns = (uint32_t)tick;
            conn->rx_epoch_ns = conn->tx_epoch_ns = t_ns;
            conn->rx_ticks = conn->tx_ticks = 0;
        }
        break;
    }
    default:
        break;
    }
}

static bool mid_tx_flush_acc(mid_t *conn)
{
    uint8_t cin = conn->tx_acc_eox
                      ? MIDI_CIN_SYSEX_END_1BYTE + conn->tx_acc_len - 1
                      : MIDI_CIN_SYSEX_START;
    if (!mid_tx_packet(conn, cin, conn->tx_acc, conn->tx_acc_len))
        return false;
    conn->tx_acc_len = 0;
    return true;
}

static bool mid_tx_run_raw(mid_t *conn)
{
    bool wrote = false;
    for (;;)
    {
        if (conn->tx_pending)
        {
            if (!mid_tx_packet(conn, mid_msg_cin(conn->tx_msg[0]), conn->tx_msg, conn->tx_msg_len))
                return wrote;
            wrote = true;
            conn->tx_pending = false;
            conn->tx_msg_len = 0;
        }
        if (conn->tx_state == MID_TX_SYSEX && (conn->tx_acc_len == 3 || conn->tx_acc_eox))
        {
            if (!mid_tx_flush_acc(conn))
                return wrote;
            wrote = true;
            if (conn->tx_acc_eox)
            {
                conn->tx_acc_eox = false;
                conn->tx_state = MID_TX_DELTA;
            }
        }
        if (conn->tx_tail == conn->tx_head)
            return wrote;
        uint8_t b = conn->tx_ring[conn->tx_tail];
        if (b >= 0xF8)
        {
            if (!mid_tx_packet(conn, MIDI_CIN_1BYTE_DATA, &b, 1))
                return wrote;
            wrote = true;
            conn->tx_tail = (conn->tx_tail + 1) & (MID_RING_SIZE - 1);
            continue;
        }
        if (conn->tx_state == MID_TX_SYSEX)
        {
            if (b >= 0x80 && b != 0xF7)
            {
                // A status byte ends the sysex, so an F7 is added to make a
                // SysEx-end packet and b is parsed again after the flush.
                conn->tx_acc[conn->tx_acc_len++] = 0xF7;
                conn->tx_acc_eox = true;
                continue;
            }
            conn->tx_tail = (conn->tx_tail + 1) & (MID_RING_SIZE - 1);
            conn->tx_acc[conn->tx_acc_len++] = b;
            if (b == 0xF7)
                conn->tx_acc_eox = true;
            continue;
        }
        conn->tx_tail = (conn->tx_tail + 1) & (MID_RING_SIZE - 1);
        if (conn->tx_msg_len && b >= 0x80)
            conn->tx_msg_len = 0;
        if (!conn->tx_msg_len)
        {
            if (b == 0xF0)
            {
                conn->tx_status = 0;
                conn->tx_acc[0] = 0xF0;
                conn->tx_acc_len = 1;
                conn->tx_acc_eox = false;
                conn->tx_state = MID_TX_SYSEX;
                continue;
            }
            if (b < 0x80)
            {
                if (!conn->tx_status)
                    continue;
            }
            else if (b >= 0xF0)
                conn->tx_status = 0; // system common cancels running status
            else
                conn->tx_status = b;
        }
        mid_tx_msg_byte(conn, b);
    }
}

static bool mid_tx_run(uint8_t idx, uint64_t now_ns)
{
    mid_t *conn = &mid_mounts[idx];
    if (mid_raw(conn))
        return mid_tx_run_raw(conn);
    bool wrote = false;
    for (;;)
    {
        if (conn->tx_rt)
        {
            if (!mid_tx_packet(conn, MIDI_CIN_1BYTE_DATA, &conn->tx_rt, 1))
                return wrote;
            wrote = true;
            conn->tx_rt = 0;
        }
        if (conn->tx_state == MID_TX_SYSEX)
        {
            for (;;)
            {
                if (conn->tx_acc_len == 3 || conn->tx_acc_eox)
                {
                    if (!mid_tx_flush_acc(conn))
                        return wrote;
                    wrote = true;
                    if (conn->tx_acc_eox)
                    {
                        conn->tx_acc_eox = false;
                        conn->tx_state = conn->tx_sysex_break ? MID_TX_MSG : MID_TX_DELTA;
                        conn->tx_sysex_break = false;
                        conn->tx_msg_len = 0;
                        break;
                    }
                }
                if (conn->tx_head == conn->tx_tail)
                {
                    conn->tx_idle = true;
                    return wrote;
                }
                uint8_t b = conn->tx_ring[conn->tx_tail];
                if (b >= 0xF8)
                {
                    if (!mid_tx_packet(conn, MIDI_CIN_1BYTE_DATA, &b, 1))
                        return wrote;
                    wrote = true;
                    conn->tx_tail = (conn->tx_tail + 1) & (MID_RING_SIZE - 1);
                    continue;
                }
                if (b >= 0x80 && b != 0xF7)
                {
                    conn->tx_acc[conn->tx_acc_len++] = 0xF7;
                    conn->tx_acc_eox = true;
                    conn->tx_sysex_break = true;
                    continue;
                }
                conn->tx_tail = (conn->tx_tail + 1) & (MID_RING_SIZE - 1);
                conn->tx_acc[conn->tx_acc_len++] = b;
                if (b == 0xF7)
                    conn->tx_acc_eox = true;
            }
            continue;
        }
        if (!mid_tx_parse(conn, now_ns))
        {
            if (conn->tx_rt)
                continue;
            conn->tx_idle = true;
            return wrote;
        }
        if (conn->tx_due_ns > now_ns)
            return wrote;
        if (conn->tx_meta)
        {
            mid_tx_apply_meta(conn);
            conn->tx_pending = conn->tx_meta = false;
            conn->tx_state = MID_TX_DELTA;
            continue;
        }
        if (conn->tx_msg[0] == 0xF0)
        {
            conn->tx_acc[0] = 0xF0;
            conn->tx_acc_len = 1;
            conn->tx_acc_eox = false;
            conn->tx_sysex_break = false;
            conn->tx_pending = false;
            conn->tx_state = MID_TX_SYSEX;
            continue;
        }
        if (!mid_tx_packet(conn, mid_msg_cin(conn->tx_msg[0]), conn->tx_msg, conn->tx_msg_len))
            return wrote;
        wrote = true;
        conn->tx_pending = false;
        conn->tx_state = MID_TX_DELTA;
    }
}

void mid_task(void)
{
    for (uint8_t itf = 0; itf < CFG_TUH_MIDI; itf++)
        mid_rx_pull_itf(itf);
    uint64_t now_ns = time_us_64() * 1000;
    uint16_t flush = 0;
    for (uint8_t idx = 0; idx < CFG_TUH_MIDI; idx++)
        if (mid_mounts[idx].mounted && mid_mounts[idx].opened &&
            mid_tx_run(idx, now_ns))
            flush |= 1u << mid_mounts[idx].itf;
    for (uint8_t itf = 0; itf < CFG_TUH_MIDI; itf++)
        if (flush & (1u << itf))
            tuh_midi_write_flush(itf);
}

int mid_status_response(char *buf, size_t buf_size, int state, unsigned)
{
    if (state < 0 || state >= CFG_TUH_MIDI)
        return -1;
    mid_t *conn = &mid_mounts[state];
    if (conn->mounted)
    {
        char devname[sizeof(mid_string) + 1];
        snprintf(devname, sizeof(devname), "%s%d", mid_string, state);
        char name[USB_DESC_STRING_MAX_CHAR_LEN + 1];
        name[0] = '\0';
        const void *desc = usb_string_fetch_product(conn->daddr);
        if (desc)
            usb_desc_string_to_oem(desc, USB_DESC_STRING_BUF_SIZE, name, sizeof(name));
        if (!name[0])
        {
            uint16_t vid = 0, pid = 0;
            tuh_vid_pid_get(conn->daddr, &vid, &pid);
            snprintf(name, sizeof(name), "%04X:%04X", vid, pid);
        }
        // name is OEM, oem_snprintf would mangle high bytes
        snprintf(buf, buf_size, STR_STATUS_MIDI, devname, name);
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

    uint32_t ppqn = 0;
    for (const char *p = &name[6]; *p; p++)
    {
        if (!isdigit((unsigned char)*p))
        {
            *err = API_EINVAL;
            return -1;
        }
        ppqn = ppqn * 10 + (*p - '0');
        if (ppqn > MID_MAX_PPQN)
        {
            *err = API_EINVAL;
            return -1;
        }
    }
    bool raw = !ppqn;

    // mid_rx_pull_itf reads an interface only while one of its input cables is
    // open, so old packets can be waiting in its RX FIFO. They are discarded
    // here unless another input cable on the same interface is open.
    bool itf_idle = true;
    for (uint8_t i = 0; i < CFG_TUH_MIDI; i++)
        if (i != idx && mid_mounts[i].mounted && mid_mounts[i].opened &&
            mid_mounts[i].has_rx && mid_mounts[i].itf == conn->itf)
            itf_idle = false;
    if (itf_idle)
    {
        uint8_t stage[64];
        while (tuh_midi_packet_read_n(conn->itf, stage, sizeof(stage)))
            ;
    }

    uint64_t now_ns = time_us_64() * 1000;
    conn->tempo = MID_DEFAULT_TEMPO;
    conn->ppqn = (uint16_t)ppqn;
    conn->tick_ns = raw ? 0
                        : (uint32_t)(((uint64_t)MID_DEFAULT_TEMPO * 1000 + ppqn / 2) / ppqn);
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
    conn->tx_rt = 0;
    conn->tx_meta = false;
    conn->tx_acc_len = 0;
    conn->tx_acc_eox = false;
    conn->tx_sysex_break = false;
    conn->opened = true;
    if (raw)
        RP6502_LOG(midi, INFO, "%d open raw", idx);
    else
        RP6502_LOG(midi, INFO, "%d open ppqn %u", idx, conn->ppqn);
    return (conn->session << 4) | idx;
}

// A descriptor holds the mount session above the slot index, so a descriptor
// kept across an unplug does not resolve to the next device mounted in that
// slot.
static mid_t *mid_desc_conn(int desc)
{
    int idx = desc & 0xF;
    if (idx >= CFG_TUH_MIDI || (desc >> 4) != mid_mounts[idx].session)
        return NULL;
    return &mid_mounts[idx];
}

static bool mid_tx_draining(const mid_t *conn)
{
    return conn->tx_head != conn->tx_tail || conn->tx_pending ||
           conn->tx_acc_eox || conn->tx_acc_len == 3;
}

static void mid_close_finalize(mid_t *conn)
{
    if (!mid_raw(conn) && conn->mounted && conn->has_tx && conn->tx_state == MID_TX_SYSEX)
    {
        if (conn->tx_acc_len > 0 && conn->tx_acc_len < 3 && !conn->tx_acc_eox)
        {
            conn->tx_acc[conn->tx_acc_len++] = 0xF7;
            conn->tx_acc_eox = true;
            if (mid_tx_flush_acc(conn))
                tuh_midi_write_flush(conn->itf);
        }
        else
        {
            uint8_t eox = 0xF7;
            if (mid_tx_packet(conn, MIDI_CIN_SYSEX_END_1BYTE, &eox, 1))
                tuh_midi_write_flush(conn->itf);
        }
    }
    conn->opened = false;
}

static std_rw_result mid_drain_gate(int desc, mid_t **conn, api_errno *err)
{
    *conn = mid_desc_conn(desc);
    if (!*conn)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    if ((*conn)->opened && (*conn)->mounted && (*conn)->has_tx && mid_tx_draining(*conn))
        return STD_PENDING;
    return STD_OK;
}

std_rw_result mid_std_close(int desc, api_errno *err)
{
    mid_t *conn;
    std_rw_result result = mid_drain_gate(desc, &conn, err);
    if (result != STD_OK)
        return result;
    RP6502_LOG(midi, INFO, "%d close", desc & 0xF);
    mid_close_finalize(conn);
    return STD_OK;
}

std_rw_result mid_std_sync(int desc, api_errno *err)
{
    mid_t *conn;
    return mid_drain_gate(desc, &conn, err);
}

std_rw_result mid_std_read(int desc, char *buf, uint32_t count,
                           uint32_t *bytes_read, api_errno *err)
{
    mid_t *conn = mid_desc_conn(desc);
    if (!conn || !conn->mounted || !conn->opened || !conn->has_rx)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    uint32_t n = 0;
    while (n < count && conn->rx_tail != conn->rx_head)
    {
        buf[n++] = conn->rx_ring[conn->rx_tail];
        conn->rx_tail = (conn->rx_tail + 1) & (MID_RING_SIZE - 1);
    }
    *bytes_read = n;
    return STD_OK;
}

std_rw_result mid_std_write(int desc, const char *buf, uint32_t count,
                            uint32_t *bytes_written, api_errno *err)
{
    mid_t *conn = mid_desc_conn(desc);
    if (!conn || !conn->mounted || !conn->opened || !conn->has_tx)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    uint32_t n = 0;
    while (n < count && mid_ring_free(conn->tx_head, conn->tx_tail))
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
            break;
        mid_t *conn = &mid_mounts[slot];
        uint8_t session = (uint8_t)(conn->session + 1);
        memset(conn, 0, sizeof(*conn));
        conn->session = session;
        conn->daddr = mount_cb_data->daddr;
        conn->itf = itf;
        conn->cable = c;
        conn->has_rx = c < rx;
        conn->has_tx = c < tx;
        conn->mounted = true;
        RP6502_LOG(midi, INFO, "%u mount itf %u cable %u %c%c %04X:%04X", slot, itf, c,
                   conn->has_rx ? 'R' : '-', conn->has_tx ? 'T' : '-', vid, pid);
    }
}

void tuh_midi_umount_cb(uint8_t itf)
{
    RP6502_LOG(midi, INFO, "unmount itf %u", itf);
    for (uint8_t i = 0; i < CFG_TUH_MIDI; i++)
        if (mid_mounts[i].mounted && mid_mounts[i].itf == itf)
        {
            mid_mounts[i].mounted = false;
            mid_mounts[i].opened = false;
        }
}

void mid_stop(void)
{
    for (uint8_t idx = 0; idx < CFG_TUH_MIDI; idx++)
        if (mid_mounts[idx].opened)
            mid_close_finalize(&mid_mounts[idx]);
}
