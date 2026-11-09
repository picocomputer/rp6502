# Hardware-in-the-loop tests of the RP6502-PICO

These tests run on a real RP6502-PICO through its console, and through a
debug probe on the RIA when one is attached. CI has no machine to run them
on, so a developer runs them on a workstation with the rig attached.

    python3 tests/hil/pico/hil.py -c .rp6502 --elf build/ria-w/rp6502-ria-w.elf tput

`hil.py --help` lists the options and the tests.

- `-c` is the settings file of `rp6502.py`, the one a project passes to
  `rp6502.py run`. Its `device` and `key` select the console. `-d` and `-k`
  give them on the command line. With a key, the tests use the telnet
  console of a Pico 2 W. Without one, they use a serial port.
- `--elf` names the ELF of the firmware on the RIA. With it, the tests read
  the audio load through the probe, and `--flash` programs that ELF before
  the tests.
- `--runs 2` runs the tests twice. Compare two builds with two runs of each
  on the same drive.
- `--emit dir` writes the ROMs. The tests assemble them from `roms.py` with
  the assembler of the test suite.

## The rig

- An RP6502-PICO. A Pico 2 W RIA gives the telnet console, which leaves the
  USB console free. A serial port works too.
- A USB drive as MSC0: for `tput` and `update`. `--dir` picks another
  directory.
- For `audio` and `--flash`, a CMSIS-DAP probe, such as the Raspberry Pi
  Debug Probe, on the SWD pins of the RIA, and OpenOCD. The tests find
  OpenOCD in `$OPENOCD`, in `~/.pico-sdk/openocd/`, where the Pico extension
  of VS Code installs it, or on the PATH. For its reads, the probe uses the
  memory access ports alone (`memap.cfg`), so the cores never halt.

## tput

The test to run after any change to the bus, DMA, the main loop, or what
runs from RAM. At each block size from 512 bytes to 16 KB, `write_xram`
writes 512 KB to a file and `read_xram` reads it back, while all eight PSG
voices, all nine OPL2 channels, and 48 kHz stereo PCM play. Only the reads
are timed, and PCM gets 4 KB after each read. The ROM prints the table. The
test fails on an error, a missing row, an overrun of the audio interrupt
(with `--elf`), or 20 s without output, which is how a 6502 crash inside
`read_xram` shows.

On the reference drive, reads of 16 KB over 1100 KB/s mean the whole read
path is healthy.

Reference: 2026-11-09, the Release RIA-W of f685ba28, a 31.4 GB SanDisk
Cruzer Blade, five runs.

| block | read KB/s |
|------:|----------:|
|   512 |   410–476 |
|  1024 |   593–653 |
|  2048 |   795–825 |
|  4096 |   919–972 |
|  8192 | 1038–1064 |
| 16384 | 1101–1115 |

The audio load during the run is 56–60%, with the peak sample at 74% and no
overruns. Writes depend on the drive and change from run to run. Another
drive gives other numbers: a 128 MB stick reads 16 KB at about 1040 KB/s.

## audio

Every sound engine at once with the bus busy: the PSG and the OPL2 as in
`tput`, the bell struck on every loop, and PCM fed 4 KB at a time with
`write_xram`, with 2048 XRAM writes at the full speed of the bus after each
4 KB. The probe reads `aud_load` for `--seconds`. The test fails on an
overrun of the audio interrupt, or when the 6502 stops. Needs `--elf`.

The load is the share of core 0, which runs 256 million cycles a second,
that the audio interrupt takes. A sample has about 5150 cycles.

Reference, same firmware: 62.3%, the peak sample at 67%, no overruns, 211
loops a second.

## bus

The 6502 writes and reads the RIA registers and checks every byte: seeded
bytes through RW0 read back through RW1, fixed bytes read back to back
through RW1, and 16 bytes pushed and pulled through the XSTACK. Run it after
changes to act_loop, the bus PIO and DMA, the registers, or bus priority.
The test fails on any mismatch.

Reference, same firmware: about 690 loops a second, no errors.

## update

The firmware update from a USB drive. The test uploads `--uf2` to the drive
and runs `FLASH`. During the update, a second telnet login must be refused.
The RIA then reboots, and the test waits for it and shows its version line.
Give it a RIA UF2: a VGA UF2 flashes the VGA, and the VGA console is the way
back when the RIA fails.

Reference, same firmware: the upload takes 31 s over telnet, the flash 13 s.

## Cautions

- A restart of the RIA while a ROM streams from a USB drive can hang the
  drive until it is replugged. `--flash` stops the ROM first. When MSC0 is
  missing after a flash, replug the drive.
- When the RIA does not come back after `--flash` or `update`, reboot it by
  hand.
