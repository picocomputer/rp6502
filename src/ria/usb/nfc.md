# NFC 6502 API Design Proposal

## Overview

The NFC device is exposed to the 6502 application through the standard
`open`/`read`/`write`/`close` interface, device name `"NFC:"`.

The PN532 reader runs autonomously in the background on the RIA processor.
The 6502 application may be slow to poll; the API guarantees it will always
receive a complete, consistent, noise-free snapshot of the NFC state — it
cannot miss data or observe a partial transfer.

**Async model**: Commands arm operations that run asynchronously. As operations
complete, they post responses to a response pool. `read()` returns the next
response from the pool and clears it; the application calls `read()` repeatedly
until it reaches `NFC_IDLE` or `NFC_NO_READER`, the quiescent states at the
bottom of the pool. Multiple commands — a read and a write — can be armed
simultaneously; each posts its own response independently.

**Snapshot invariant**: `NFC_DATA` carries NDEF payload. At the moment the
first `read()` call returns `NFC_DATA`, `nfc_ndef_buf` is copied into
`nfc_snap_buf`. The application then drains `nfc_snap_buf` across as many
`read()` calls as needed, while the PN532 continues running freely.


## open / close

```c
int fd = open("NFC:", O_RDWR);
```

- Only one file descriptor may be open at a time; a second `open()` returns
  `EBUSY`.
- `close()` disarms any pending write command and releases the descriptor.
  The PN532 hardware continues running.
- While open, `pro_nfc()` is not called; the 6502 application receives NDEF data
  directly via `read()`.


## write() — Commands

A single `write()` call sends one command. Commands are:

| Bytes written | Meaning |
|---------------|---------|
| `NFC_CMD_READ` (1 byte) | Snapshot current NFC state and post the result immediately. |
| `NFC_CMD_WRITE, lenLo, lenHi, payload…` | Arm a write with an NDEF payload. |
| `NFC_CMD_CANCEL` (1 byte) | Disarm any armed write. |
| `NFC_CMD_SUCCESS1` (1 byte) | Play `bel_nfc_success_1`. |
| `NFC_CMD_SUCCESS2` (1 byte) | Play `bel_nfc_success_2`. |
| `NFC_CMD_ERROR` (1 byte) | Play `bel_nfc_fail`. |

### Command semantics

**`NFC_CMD_READ`** immediately snapshots the current PN532 state and posts the
corresponding response to the pool. The application must re-arm with another
`NFC_CMD_READ` to poll again.

**`NFC_CMD_WRITE`** arms a write. The application streams the opcode, length,
and payload across as many `write()` calls as needed; the driver accumulates
bytes into a staging buffer and arms the write once the full payload has
arrived. The NDEF payload is delivered to the current card if one is present,
or to the next card that is presented. When the write eventually completes,
`NFC_WRITE_SUCCESS` or `NFC_WRITE_ERROR` is posted to the pool. A second
`NFC_CMD_WRITE` while one is already armed silently overwrites it (last write wins).

**`NFC_CMD_CANCEL`** immediately disarms any armed write. No response is posted.

**`NFC_CMD_SUCCESS1`** plays `bel_nfc_success_1`. No response is posted.

**`NFC_CMD_SUCCESS2`** plays `bel_nfc_success_2`. No response is posted.

**`NFC_CMD_ERROR`** plays `bel_nfc_fail`. No response is posted.

**Concurrency**: Any combination of commands may be armed simultaneously.
`NFC_CMD_READ` responses are always delivered before any pending write
responses, so the `write(NFC_CMD_READ); read()` pattern works synchronously
between async `NFC_CMD_WRITE` operations.

## read() — Responses

The application always begins by reading **1 byte** — the result type.
Only `NFC_DATA` has additional bytes following it.

| Result byte | Additional bytes | Posted by |
|-------------|-----------------|----------|
| `NFC_IDLE` | — | Bottom of pool; no responses armed |
| `NFC_NO_READER` | — | Persistent floor when no reader is attached |
| `NFC_NO_CARD` | — | `NFC_CMD_READ` when no card is present |
| `NFC_CARD_INSERTED` | — | `NFC_CMD_READ` when card is present but NDEF not yet/no longer available |
| `NFC_READ_ERROR` | — | `NFC_CMD_READ` when NDEF read failed |
| `NFC_DATA` | 4 bytes CC, `uint8_t age_ds`, `uint8_t lenLo`, `uint8_t lenHi`, then `len` NDEF bytes | `NFC_CMD_READ` when card has been read (len may be 0 for blank/null NDEF) |
| `NFC_WRITE_SUCCESS` | — | `NFC_CMD_WRITE` when write completed successfully |
| `NFC_WRITE_ERROR` | — | `NFC_CMD_WRITE` when write failed |

`age_ds` is the age of the card data in tenths of a second (0.1 s resolution),
capped at 255 (25.5 s). If the data is older than 25.5 seconds it is discarded
and `NFC_CARD_INSERTED` is posted instead of `NFC_DATA`.

`read()` returns the next response from the pool, clearing it. The application
calls `read()` repeatedly until it receives all expected responses.


### What each command posts

`NFC_CMD_READ` snapshots current PN532 state and posts one of:

| PN532 state at snapshot time | Response posted |
|-----------------------------|----------------|
| No reader attached | `NFC_NO_READER` |
| Reader attached, no card present | `NFC_NO_CARD` |
| Card present, not yet read | `NFC_CARD_INSERTED` |
| Card successfully read, data ≤ 25.5 s old, not yet delivered | `NFC_DATA` (len may be 0) |
| Card read failed ≤ 25.5 s ago | `NFC_READ_ERROR` |

`NFC_CMD_WRITE` arms a write and posts one of:

| Condition | Response posted |
|-----------|----------------|
| Write completed successfully | `NFC_WRITE_SUCCESS` |
| Write failed | `NFC_WRITE_ERROR` |

`NFC_CMD_CANCEL` disarms any armed write and posts nothing.

`NFC_CMD_SUCCESS1`, `NFC_CMD_SUCCESS2`, and `NFC_CMD_ERROR` play sounds and post nothing.

### Snapshot rule

At the moment the first `read()` call is made after a result is ready,
`nfc_ndef_buf` is copied into `nfc_snap_buf`. The 6502 drains `nfc_snap_buf`
while the PN532 state machine is free to continue using `nfc_ndef_buf`
immediately. A slow 6502 application is guaranteed to read consistent data
even if the card is removed or a new card is presented while the application
is mid-read.

### Buffer layout

Three NTAG216-sized (888 byte) buffers are required:

| Buffer | Purpose |
|--------|---------|
| `nfc_ndef_buf` | Live PN532 read buffer — written by the PN532 state machine |
| `nfc_snap_buf` | Snapshot frozen at first `read()` call — drained by the 6502 |
| `nfc_write_buf` | Staging for incoming `NFC_CMD_WRITE` payload — streamed in from the 6502 |
