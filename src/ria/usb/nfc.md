# NFC Device API

Device name: `"NFC:"`

The PN532 reader runs autonomously on the RIA. The 6502 sends commands via
`write()` and receives responses via `read()`. Commands post responses to a
two-slot priority pool; `read()` returns the highest-priority pending response
and clears it. Read responses have priority over write responses. When the
pool is empty, `read()` returns the floor state (`NFC_RESP_IDLE` or
`NFC_RESP_NO_READER`).

While the file is open, automatic ROM launch is suppressed.

## open / close

```c
int fd = open("NFC:", O_RDWR);
```

## write() -- Commands

| Byte | Command |
|------|---------|
| `NFC_CMD_READ` (0x01) | Snapshot NFC state, post result |
| `NFC_CMD_WRITE` (0x02), lenLo, lenHi, payload... | Arm a tag write (streamable across calls) |
| `NFC_CMD_CANCEL` (0x03) | Disarm pending write |
| `NFC_CMD_SUCCESS1` (0x04) | Play success tone 1 |
| `NFC_CMD_SUCCESS2` (0x05) | Play success tone 2 |
| `NFC_CMD_ERROR` (0x06) | Play error tone |

`NFC_CMD_WRITE` streams opcode + length + payload across multiple `write()`
calls. Once the full payload arrives, the write is armed. It executes on the
current card or the next one presented. A second `NFC_CMD_WRITE` overwrites
the first (last write wins).

The driver plays tones automatically except on `NFC_RESP_READ_SUCCESS` and
`NFC_RESP_WRITE_SUCCESS`, where the 6502 application decides the outcome.

## read() -- Responses

Read **1 byte** for the result type. Only `NFC_RESP_READ_SUCCESS` has a trailing
payload.

| Result | Extra bytes | Source |
|--------|-------------|--------|
| `NFC_RESP_IDLE` (0x00) | -- | Pool empty, reader attached |
| `NFC_RESP_NO_READER` (0x01) | -- | Pool empty, no reader |
| `NFC_RESP_NO_CARD` (0x02) | -- | CMD_READ: no card |
| `NFC_RESP_CARD_INSERTED` (0x03) | -- | CMD_READ: card present, NDEF not available |
| `NFC_RESP_READ_ERROR` (0x04) | -- | CMD_READ: NDEF read failed |
| `NFC_RESP_READ_SUCCESS` (0x05) | 7 header + *len* NDEF | CMD_READ: card data available |
| `NFC_RESP_WRITE_SUCCESS` (0x06) | -- | CMD_WRITE: done |
| `NFC_RESP_WRITE_ERROR` (0x07) | -- | CMD_WRITE: failed |

### NFC_RESP_READ_SUCCESS header (7 bytes)

| Offset | Field | Description |
|--------|-------|-------------|
| 0 | `age_ds` | Data age in 0.1 s, capped at 255. Data older than 25.5 s yields `NFC_RESP_CARD_INSERTED` instead. |
| 1-4 | CC[4] | Capability Container from page 3. CC[2] * 8 = max NDEF bytes. |
| 5-6 | `lenLo, lenHi` | NDEF payload length (little-endian). |

NDEF payload follows the header and may span multiple `read()` calls.
