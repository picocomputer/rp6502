# PCM audio driver

Adds PCM audio playback to the firmware and emulator, supporting mono/stereo
samples, 8/16-bit formats, resampling, and **simultaneous playback with PSG or
OPL**. Includes emulator save-state support and audio mixing tests. **Switching
between PSG and OPL leaves PCM playing**; stopping the program stops PCM too.

## Memory impact

Measured against `main` (`ad6293c`) for `driver/PCM` (`8243a44`), using a
Pico 2 W / RP2350 Release build with GCC 15.2.1, Pico SDK 2.3.0 and LTO:

| Resource | `main` | With PCM | Increase |
| --- | ---: | ---: | ---: |
| Flash image | 614,372 B | 615,012 B | **640 B** |
| Main RAM occupied before the heap | 483,868 B | 484,540 B | **672 B** |

The RAM increase includes code copied into RAM. This leaves **39,748 B
(38.8 KiB) for the heap before runtime allocations**; the core stacks use
separate RAM banks. VGA firmware is unchanged.

Playback uses a **512-byte to 8-KiB ring buffer plus an 8-byte header within
the existing 64-KiB XRAM**. The default ring size is 1 KiB. This consumes XRAM
available to the 6502 program without adding a separate firmware allocation.
The figures above do not measure peak runtime stack or heap usage.

## Configuration

Use `xreg(0, 1, 2, base)` to start PCM. `base` is a **4-byte-aligned XRAM
address**, and the header and complete ring must fit within XRAM. Use
`xreg(0, 1, 2, 0xFFFF)` to stop PCM without stopping PSG or OPL.

All 16-bit fields and samples are little-endian:

| Offset from `base` | Size | Meaning |
| --- | ---: | --- |
| `+0` | 2 B | Producer's `write_ptr`, a byte offset within the ring |
| `+2` | 1 B | Format: bit 0 = mono, bit 1 = 8-bit, bit 2 = unsigned 8-bit |
| `+3` | 1 B | Ring size as log2: 9 through 13; other values select 10 |
| `+4` | 2 B | Sample rate in Hz |
| `+6` | 2 B | Consumer's `read_ptr`, published by the driver |
| `+8` | Ring size | Interleaved sample data; left then right for stereo |

Supported rates are **8000, 11025, 16000, 22050, 32000 and 44100 Hz**.
Other values, including zero, select 44100 Hz. The driver resamples to the
mixer's native rate using linear interpolation. Sixteen-bit samples are
signed; bit 2 affects only 8-bit samples. Mono samples feed both channels.

Pointers are byte offsets modulo the ring size, aligned to a complete frame.
Equal pointers mean empty, so leave at least one frame unused. Publish the
write pointer only after writing the corresponding samples. Starting PCM
resets its internal read pointer to zero. When the buffer empties, playback
holds the last sample; append silence or explicitly stop PCM when finished.

## cc65 example

Save the following as `pcm_demo.c`. It plays about one second of a 250 Hz
square wave at 8000 samples/s, followed by a short silence tail. It uses
unsigned 8-bit mono samples and reserves **XRAM `$4000` through `$6007`**:
an 8-byte header and an 8192-byte ring. Do not place other data in that range.

The program queues 8000 samples before starting playback, so the write pointer
never changes while the consumer runs. This avoids concurrent publication of
a two-byte write pointer and keeps the example independent of streaming speed.
It requires firmware or an emulator built with `driver/PCM`.

```c
#include <rp6502.h>
#include <stdio.h>

#define PCM_BASE       0x4000u
#define PCM_RING       (PCM_BASE + 8u)
#define PCM_SAMPLES    8000u
#define PCM_TONE       7936u

/* Read the consumer pointer via the second XRAM port. */
static unsigned pcm_read_ptr(void)
{
    unsigned char hi, lo, again;

    RIA.step1 = 0;
    do {
        RIA.addr1 = PCM_BASE + 7u;
        hi = RIA.rw1;
        RIA.addr1 = PCM_BASE + 6u;
        lo = RIA.rw1;
        RIA.addr1 = PCM_BASE + 7u;
        again = RIA.rw1;
    } while (hi != again);
    return ((unsigned)hi << 8) | lo;
}

int main(void)
{
    unsigned i;

    /* Stop any earlier PCM before changing its configuration. */
    if (xreg(0, 1, 2, 0xFFFF) < 0) {
        puts("PCM is unavailable; use firmware with driver/PCM.");
        return 1;
    }

    RIA.step0 = 1;
    RIA.addr0 = PCM_BASE;
    RIA.rw0 = (unsigned char)PCM_SAMPLES;        /* write_ptr low */
    RIA.rw0 = (unsigned char)(PCM_SAMPLES >> 8); /* write_ptr high */
    RIA.rw0 = 0x07;                            /* mono, 8-bit, unsigned */
    RIA.rw0 = 13;                              /* 8192-byte ring */
    RIA.rw0 = 0x40;                            /* 8000 Hz, low byte */
    RIA.rw0 = 0x1F;                            /* 8000 Hz, high byte */
    RIA.rw0 = 0;                               /* read_ptr low */
    RIA.rw0 = 0;                               /* read_ptr high */

    RIA.addr0 = PCM_RING;
    for (i = 0; i < PCM_TONE; ++i) {
        /* 32 samples per period: 8000 / 32 = 250 Hz. */
        RIA.rw0 = (i & 16u) ? 160 : 96;
    }
    for (; i < PCM_SAMPLES; ++i) {
        RIA.rw0 = 128;                         /* unsigned 8-bit silence */
    }

    puts("Playing PCM: 250 Hz, 8000 samples/s.");
    if (xreg(0, 1, 2, PCM_BASE) < 0) {
        puts("PCM configuration failed.");
        return 1;
    }

    /* No ring wrap in this example; the final pointer stays at 8000. */
    while (pcm_read_ptr() != PCM_SAMPLES) {
    }

    if (xreg(0, 1, 2, 0xFFFF) < 0) {
        puts("PCM stop failed.");
        return 1;
    }
    puts("PCM playback complete.");
    return 0;
}
```

With cc65's `rp6502` target installed, run these commands from the repository
root with `pcm_demo.c` in that directory:

```sh
cl65 -t rp6502 -O -o pcm_demo.bin pcm_demo.c
python tools/rp6502.py create -a 0x0200 -r 0x0200 -o pcm_demo.rp6502 pcm_demo.bin
```

These addresses match the standard cc65 `rp6502.cfg`, which places the startup
code at `$0200`. If using a custom linker configuration, adjust them to match.

Load `pcm_demo.rp6502` on a Picocomputer running the PCM firmware, or launch it
with an emulator built from this branch:

```sh
rp6502-emu pcm_demo.rp6502
```

For continuous streaming and mixing with OPL or PSG, see
[`tests/cpu/aud/pcm_opl.py`](tests/cpu/aud/pcm_opl.py) and
[`tests/cpu/aud/pcm_opl_or_psg.py`](tests/cpu/aud/pcm_opl_or_psg.py).
These examples publish the write pointer in 256-byte blocks by changing only
its high byte, avoiding a torn two-byte write while PCM is consuming samples.
