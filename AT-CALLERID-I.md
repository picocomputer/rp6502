# Add AT+VCID caller ID and ATI

`AT+VCID=1` enables caller ID. After the first `RING` of an incoming call the
modem reports the caller's IP address in the `NMBR` field, with the date and
time from the RIA clock:

```
RING

DATE = 1107
TIME = 1405
NMBR = 192.168.1.42

RING
```

The block is printed once per call, in both verbose and numeric modes, and is
suppressed by `Q1`. With `S0=1` the call is answered on the first ring, so no
block is shown. `AT+VCID=0` (factory default) disables it, `AT+VCID?` reports
the setting, `AT&W` stores it and `AT&V` shows it.

`ATI` reports the modem's own IPv4 address, or `(none)` when Wi-Fi has no
address, so a 6502 program can tell a caller where to connect.

Also preserves data received before an incoming call is answered and retries
buffered receive data promptly.

Memory impact versus `main` (commit f412f53) (RIA-W on Pico 2 W / RP2350,
Release with GCC 15.2.1, Pico SDK 2.3.0 and LTO): **+596 bytes Flash and +648
bytes static RAM**, leaving **42,432 bytes (41.4 KiB) for the heap before
runtime allocations**. The RAM increase includes code copied into RAM. VGA
firmware is unchanged; peak runtime stack and heap usage were not measured.
