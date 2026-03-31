# NFC Device API

Device name: `"NFC:"`

The PN532 reader runs autonomously on the RIA. The 6502 arms operations via
`write()` and polls results via `read()`. `NFC_CMD_READ` arms a read;
`NFC_CMD_WRITE` arms a write. Each posts to its own response slot when done.
Until a response is ready, `read()` returns the current NFC floor state
(`NFC_RESP_NO_READER`, `NFC_RESP_NO_CARD`, or `NFC_RESP_CARD_INSERTED`).
`NFC_CMD_CANCEL` disarms both.

While the `"NFC:"` device is open, automatic ROM launching is suppressed.

## open / close

```c
int fd = open("NFC:", O_RDWR);
```

## write() -- Commands

| Byte | Command |
|------|---------|
| `NFC_CMD_WRITE` (0x01), lenLo, lenHi, NDEF... | Arm a write |
| `NFC_CMD_READ` (0x02) | Arm a read |
| `NFC_CMD_CANCEL` (0x03) | Disarm both pending read and write |
| `NFC_CMD_SUCCESS1` (0x04) | Play success tone 1 |
| `NFC_CMD_SUCCESS2` (0x05) | Play success tone 2 |
| `NFC_CMD_ERROR` (0x06) | Play error tone |

`NFC_CMD_WRITE` streams the command byte + length + NDEF payload across multiple `write()`
calls. Once the full payload arrives, the write is armed. It executes on the
current card or the next one presented. A second `NFC_CMD_WRITE` overwrites
the first (last write wins).

## read() -- Responses

Read **1 byte**. `read()` always returns immediately. Only `NFC_RESP_READ`
has a trailing payload.

| Result | Extra bytes | Meaning |
|--------|-------------|--------|
| `NFC_RESP_NO_READER` (0x01) | -- | Floor: no reader attached |
| `NFC_RESP_NO_CARD` (0x02) | -- | Floor: no card present |
| `NFC_RESP_CARD_INSERTED` (0x03) | -- | Floor: card present, NDEF not ready |
| `NFC_RESP_WRITE` (0x04) | -- | Armed write complete |
| `NFC_RESP_READ` (0x05) | 7 byte header + NDEF | Armed read complete |

`NFC_RESP_READ` and `NFC_RESP_WRITE` should be followed with one or more tone
commands, or the application's own sounds.

### NFC_RESP_READ header (7 bytes)

| Offset | Field | Description |
|--------|-------|-------------|
| 0 | `age_ds` | Data age in 0.1 s, data older than 25.5 s is lost. |
| 1-4 | CC[4] | Capability Container from page 3. CC[2] * 8 = max NDEF bytes. |
| 5-6 | `lenLo, lenHi` | NDEF payload length. |

NDEF payload follows the header and may span multiple `read()` calls.
