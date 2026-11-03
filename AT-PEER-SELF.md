# Add AT+PEER? and AT+SELF?

Adds `AT+PEER?` and `AT+SELF?` to report the remote and local IP addresses
and ports of a modem call, retaining this information after disconnection.
Incoming call endpoints are available while ringing. Also preserves data
received before an incoming call is answered and retries buffered receive
data promptly.

Memory impact versus `main` (commit ad6293c) (RIA-W on Pico 2 W / RP2350, Release with GCC
15.2.1, Pico SDK 2.3.0 and LTO): **+420 bytes Flash and +480 bytes static
RAM**, leaving **39,940 bytes (39.0 KiB) for the heap before runtime
allocations**. The RAM increase includes code copied into RAM. VGA firmware
is unchanged; peak runtime stack and heap usage were not measured.
