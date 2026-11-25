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
- `-d rtt`, or `-d rtt:SERIAL` with several probes, uses the console on a
  debug probe instead; see below.
- `--elf` names the ELF of the firmware on the RIA. With it, the tests read
  the audio load through the probe, and `--flash` programs that ELF before
  the tests. `--vga-elf` names the VGA's, for the reads of `term` and
  `termload` through a probe on the VGA.
  With two probes, `--probe` picks one by its serial number. `--flash`
  writes nothing unless the chip already runs firmware of the ELF's name
  (src/host/pico/check.cfg), so a RIA build never lands on the VGA.
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
  Debug Probe, on the SWD pins of the RIA, and OpenOCD. For `--vga-elf`,
  another on the SWD pins of the VGA. The tests find OpenOCD in `$OPENOCD`,
  in `~/.pico-sdk/openocd/`, where the Pico extension of VS Code installs
  it, or on the PATH. For its reads and writes, a probe uses the memory
  access ports alone (`src/host/pico/memap.cfg`), so the cores never halt.
- For `--video`, a capture device on the VGA output, such as a UVC HDMI
  capture card behind a VGA-to-HDMI converter, with ffmpeg and NumPy.

## The console on a debug probe

A firmware built with `-DRP6502_RTT_STDIO=ON` keeps the console in RAM as
SEGGER RTT channel 0, where a probe on the SWD pins reads and writes it. It
is one more end of the console's UART: it carries what the UART carries, at
115200 baud, with nothing between the tests and the machine but the probe.
The telnet console holds a program's output to the pace of the network,
about half of that over Wi-Fi, for as long as a session is connected.

    cmake -S . -B build-rtt -DRP6502_RTT_STDIO=ON -DRP6502_RTT_HID=ON
    python3 tests/hil/pico/hil.py -d rtt:SERIAL --probe SERIAL \
        --elf build-rtt/ria-w/rp6502-ria-w.elf term sizes bus audio tput

The option is off in a release. The RIA and the VGA both take it, and a
probe on either reaches the same console. A break is a word the probe sets
beside the rings, since RTT has none. `RP6502_RTT_HID` is the option of the
next section. Each of the two builds without the other.

The rig reads the rings itself (`RttDevice` in `src/core/build.py`),
through the same OpenOCD and the same memory access ports as its other
reads. The output ring is 256 bytes, 22 ms of a busy console, and the
firmware drops and counts what does not fit. OpenOCD's own `rtt server`
reads every 100 ms unless `rtt polling_interval` says otherwise, and has to
attach to the cores.

## Keyboards, mice and gamepads on a debug probe

A RIA built with `-DRP6502_RTT_HID=ON` takes human input devices from a
probe on its SWD pins, so a test types, moves a mouse and holds a gamepad
with nothing on the USB port and nobody at the machine. The option is off
in a release, and off it adds nothing to the firmware. The VGA has no input
devices and does not take it.

A device is its HID report descriptor. The rig mounts one as USB does when
a device is plugged in, through the same parser and the same drivers, and
then sends its reports, so the firmware knows no more of a device than it
knows of a USB one. An XInput gamepad is not HID and cannot be mounted. The
firmware times nothing: a key held is a report, a wait in the test, and
another report.

`RttHid` in `src/core/build.py` is the rig's side, and
`src/host/pico/rtt_hid.h` describes the messages between the two. It takes
a `Probe` on the RIA, which needs no ELF for this, and shares the one
OpenOCD on that probe with the console and with the reads by symbol:

    probe = build.Probe(None, serial, board)
    with probe, build.RttHid(probe) as hid:
        keyboard = hid.keyboard()
        keyboard.tap(keyboard.LEFT_SHIFT, 0x04)
        keyboard.down(0x04)
        time.sleep(1)
        keyboard.up()
        mouse = hid.mouse()
        mouse.move(20, -10)
        mouse.buttons(mouse.LEFT)
        pad = hid.gamepad()
        pad.set(buttons=pad.A | pad.START, dpad=pad.UP, lx=-128, rt=255)
        other = hid.mount(descriptor, vendor_id, product_id)
        other.report(data)
        other.unmount()

- `RttHid(probe)` opens with `with`, or with `open()` and `close()`. It
  finds the firmware's structure in RAM by its id and unmounts what an
  earlier run left mounted. Closing unmounts every device.
- `hid.mount(descriptor, vendor_id=0, product_id=0)` mounts any descriptor
  and returns a `HidDevice`. `report(data)` sends a report as the device
  sends it over USB, with its report id first when the descriptor has ids.
  `unmount()` unplugs it. `slot` is its hid slot in the RIA, and `claims`
  the drivers that took it: `RttHid.KEYBOARD`, `MOUSE`, `TABLET` and `PAD`.
- `hid.keyboard()` is a standard keyboard of six keys and the modifiers.
  `down(*keys)` and `up(*keys)` hold keys and let them go, and `up()` lets
  go of all. `tap(*keys, hold=0)` does both. A key is its HID usage, as
  `src/core/hid/usage.h` has them, and the modifiers have names, such as
  `LEFT_CTRL`. Nothing turns text into keys: what a key types is up to the
  layout of the RIA.
- `hid.mouse()` is a mouse of five buttons. `move(dx, dy, wheel, pan)` moves
  it by counts, and `buttons(mask)` holds `LEFT`, `RIGHT`, `MIDDLE`, `BACK`
  and `FORWARD`. As with a USB mouse, it also moves the pointer of the
  tablet and of the console.
- `hid.gamepad()` is a gamepad of sixteen buttons, a hat, two sticks and two
  triggers. `set()` changes any of `buttons`, `dpad`, `lx`, `ly`, `rx`,
  `ry`, `lt` and `rt` and sends the whole state. The buttons and the dpad
  are the bits a program reads, with names such as `A` and `UP`. It is the
  player of the lowest number that has no gamepad. A program that maps the
  gamepads blanks them until the next report, and `set()` with nothing
  sends one.
- `hid.nop()` sends a message that does nothing and returns the seconds
  until the RIA acknowledged it.
- `hid.seats` lists every seat as the last acknowledgment gave it: the hid
  slot of the device there, or `None`, and its drivers.

Every message is acknowledged, and one that is not within `RttHid.TIMEOUT`,
2 s, raises. A mount no driver takes raises, as does a standard device its
own driver has no room for. A reboot of the RIA unmounts everything, and
the next report to a device that was mounted raises.

There are eight seats, and a descriptor or a report is 512 bytes at most.
The devices share the RIA's 16 hid slots, four keyboards, four mice and
four gamepads with those on USB and Bluetooth. The monitor's `STATUS` does
not count them.

On a Release RIA-W the option costs about 0.6 KB of RAM, 536 bytes of it
for the message and the seats, and about 0.2 KB of flash.

## The console on a capture device

With `--video`, a test reads the console off the display: the character in
each cell, and whether the cell is drawn reversed, as reverse video and the
highlight of a selection are. With the keyboard and the mouse of a debug
probe, a test then works the console as a person does and sees what that
person sees.

`screen.py` is the reader. Inside the `with` of a probe that has the ELF of
its chip:

    camera = screen.Camera("/dev/video0", probe)
    s = camera.look()
    s.text[0]         # the top row, a character for each of its 80 cells
    s.line(0)         # the same without the blanks that end it
    s.reverse[0][5]   # whether the cell at column 5 is drawn reversed
    s.offset          # where the picture is in the capture, such as (1.0, 0.0)

A character is its code in the console's code page, as `chr()` has it. A
cell that is no glyph reads as `screen.UNREADABLE`: one under the pointer,
or under the cursor while it is lit. `line()` leaves out the cursor's cell
after the text. Italics, underlines and the DEC character sets are not read. The screen is
read by brightness alone, so text or a background much dimmer than the
brightest thing on the screen, such as blue or red beside white, reads as a
blank. A screen with no text on it is not read, because there is no glyph to
find the picture's place by, and the error says so.

The reader holds the capture to three rules, and raises when one is broken.

- **The board says what it is showing.** The size is never taken from the
  capture. The display type, where the console is placed, and the code page
  are read from the RAM of a chip through its probe: the VGA's with
  `--vga-elf`, or else the RIA's copy with `--elf`. `SET VGA 0` is 640x480
  with 30 rows, and `SET VGA 2` is 1280x1024 with 32 rows, each pixel of
  the console two by two. `SET VGA 1` is refused, because nothing has
  captured a 16:9 display yet. The VGA draws the console there with the
  timing of `SET VGA 0`, so adding it to `DISPLAYS` in `screen.py` should
  be all it takes. A program that selects a canvas of its own is refused
  too: only the console canvas is read.
- **The capture is the display's size, or a whole multiple of it.** The
  reader asks the device for its YUYV sizes, through ffmpeg, and takes the
  display's own, or the smallest whole multiple, such as 1280x960 for
  640x480. A device with neither is refused, since any other size mixes
  the pixels.
- **The picture is within one pixel of true.** A converter can put the
  picture a pixel of the display to the left or to the right of where it
  belongs, and the reader allows the same up and down. It tries every place
  from one pixel one way to one pixel the other, in steps of a pixel of the
  capture, and reads the first place where the most cells are glyphs.
  `offset` is that place, in pixels of the display right and down of true.
  A column or a row the shift pushed off the capture is lost, and its cells
  are read from the pixels that are left. The pointer and the cursor cover
  a few cells. When more than 16 cells with ink, `COVERED_MAX`, are no
  glyph at the best place, the picture is further off or is not sharp, and
  the error says the fault is in the capture.

The glyphs are the firmware's: `src/core/term/font.py` builds them from
`src/core/term/font.c` each time, for the code page the board has. A cell
is a glyph only when every pixel of it is that glyph's, either way round,
and the way round is whether it is reversed, so a bright blank is a
reversed space. Only the luma is read, which YUYV has for every pixel. A
stream starts with black frames, which are skipped, and the mean of 8
frames is read.

Reference: 2026-11-24, a UVC HDMI capture card behind a VGA-to-HDMI
converter. `SET VGA 2` captured at 1280x1024 has the picture one pixel to
the right, an offset of (1.0, 0.0). `SET VGA 0` has no 640x480 on that
device and is captured at 1280x960, doubled and smoothed by the device,
with an offset of (1.0, 0.5).

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

The reference has no Bluetooth device connected. A connected one costs read
speed, because the main loop fetches each of its packets from the CYW43439
over SPI and waits for every transfer. A BLE gamepad at rest still sends
about 290 packets a second, which takes about 4% of core 0, and 16 KB reads
drop to about 945 KB/s. A faster SPI clock barely helps, so the cost is in
the number of transfers rather than their length. `SET BLE 0` gives the
reference back.

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

## term

The test to run after any change to the console's terminal, term.c on the
VGA. It sends the slowest things the terminal does at the full 115200 baud of
the console: a long REP, insert mode, ICH and DCH, erases, IL and DL, a
scroll storm, palette changes, RIS, the alternate screen, a long SGR, and
drawn controls. A probe follows each one at once: the cursor homed to row 2,
200 x's, and a cursor position request. The UART keeps sending while the
terminal works, so a byte that the VGA's UART dropped puts the cursor
somewhere else, and the test fails.

    python3 tests/hil/pico/hil.py -d 192.168.1.93 -k key --video /dev/video0 term

With `--video`, a capture device on the VGA output is watched during the run,
every frame, and a late scanline fails the test. A late scanline is a row of
scanvideo's missing-line blue, so the test draws no blue of its own.

With `--vga-elf`, the ELF of the firmware on the VGA, a probe on the SWD
port of the VGA is read after the run, and `--vga-probe` picks it by its
serial number when there are several. Two things fail the test then: a
console byte that reached the VGA's terminal while it still had rows of an
operation to do (`draw_bytes_in_jobs`), which the RIA's hold is there to
prevent, and a word the PIX register FIFO had no room for (RXSTALL of state
machine 1 in PIO1's FDEBUG). The test clears both before the run.

    python3 tests/hil/pico/hil.py -d 192.168.1.93 -k key --vga-elf \
        build/vga/rp6502-vga.elf --vga-probe SERIAL term termload

With `--elf` too, and so a probe on each chip, the run ends with one more
read: the RIA's copy of the console's terminal and the VGA's must be the
same, in their sizes, their cursors, and the rings their rows are laid out
in. The RIA names rows of that ring to the VGA, for the selection, so a
difference shows there as a highlight on the wrong rows. `check_console` in
`hil.py` is that read, for any test that needs it somewhere else.

## termload

The term test with the VGA as busy as a program can make it: a 640x480
canvas with two 8-bit bitmaps in mode 3 over the whole of it, and the
terminal in mode 0 above them. Run it with `--video`, since the late
scanlines are what it finds.

    python3 tests/hil/pico/hil.py -d 192.168.1.93 -k key --video /dev/video0 termload

## select

The selection the console's mouse makes is the RIA's, which sends it to the
VGA in two words of PIX once a frame, in order with the console's bytes. The
test reads the selection of both chips through their probes, so it needs
`--elf` and `--vga-elf`, and someone at the machine: it asks for a drag
across the text on the console with a USB mouse on the RIA, and waits a
minute for one. The two chips' consoles must be the same first, as after
`term`. Then the RIA and the VGA must hold the same selection, as
made, after monitor output that scrolls it, and after output that scrolls
its rows away, which drops it on both.

    python3 tests/hil/pico/hil.py -d 192.168.1.93 -k key \
        --elf build/ria-w/rp6502-ria-w.elf --probe RIA_SERIAL \
        --vga-elf build/vga/rp6502-vga.elf --vga-probe VGA_SERIAL select

## hid

The test to run after any change to the input devices of a debug probe,
`rtt_hid.c` and `RttHid`. It needs a RIA built with `RP6502_RTT_HID`, a
probe on it, and no ELF. It mounts a keyboard, taps the A key at the
monitor's prompt, reads the echo from the console, and erases it with
Backspace. The key is HID usage 4, which the layout of the RIA turns into a
character, so the test expects a US layout. Then it times 100 messages that
do nothing, there and back, with the console's polls running beside them
when the console is on the same probe.

    python3 tests/hil/pico/hil.py -d rtt hid

With several probes, name the RIA's in both places:

    python3 tests/hil/pico/hil.py -d rtt:SERIAL --probe SERIAL hid

The probe is `--probe`'s. With the console on the same probe, both name it
by the same serial number, as above, so that they share one OpenOCD. Any
other console works too.

The test fails when no keyboard mounts, the echo is not `a`, or a message
is not acknowledged. The times have no reference yet.

## editkeys, pasteline, pastelines

The tests of the line editor on the machine itself, to run after any change
to its keys, the clipboard, or the console's mouse. A keyboard and a mouse
on the RIA's debug probe do what a person would, and `--video` shows what
that person would see. They need a RIA built with `RP6502_RTT_HID`, a probe
on it, `--video`, and `--elf` or `--vga-elf` for the size of the display.

    python3 tests/hil/pico/hil.py -d rtt:SERIAL --probe SERIAL \
        --elf build-rtt/ria-w/rp6502-ria-w.elf --video /dev/video0 \
        editkeys pasteline pastelines

Each test first fills the screen: 34 times it types two letter keys and
Ctrl-C, which ends a line without entering it, so every row reads the same,
such as `]zq^C`, and the prompt is on the last row. The letters are picked
afresh on every run, so what a test pastes can only be what that run copied
or cut, never what an earlier run left on the clipboard. The display must show
exactly that before the test goes on. A row copied from that screen starts
with the prompt's `]`, which no command, ROM, or address starts with, so
the only thing a paste can run is an unknown command. Each test ends with
Ctrl-C at the prompt, and leaving a test unmounts the keyboard and the
mouse, which lets go of anything held. Each prints where it found the
picture in the capture.

- `editkeys`: types three letters, selects the last two with Shift-Left
  twice, and then sends Ctrl-Shift-X, a fourth letter, and Ctrl-Shift-V
  with no wait between them. The prompt must read the first letter, the
  fourth, and then the two that were cut: the cut, the key, and the paste
  took effect in order, and the paste was what the cut took. The keys are HID usages, so
  the test expects a US layout.
- `pasteline`: triple-clicks the top line of the screen. The display must
  show that whole row highlighted, but for the cells under the pointer, and
  no other. After Ctrl-Shift-C and Ctrl-Shift-V the prompt must read `]`
  and the line, on the last row still: a paste of one line waits for Enter.
- `pastelines`: drags from the top line into the second, which the display
  must show highlighted from the start of the first to partway into the
  second. After copy and paste, the last three rows must read the first
  line at a prompt, the monitor's answer to it, which starts with `?`, and
  the part of the second line waiting at the next prompt.

The mouse moves by counts, two to a pixel of the console's canvas, so a
test sends it far up and left to the corner before it goes anywhere.

## Cautions

- A restart of the RIA while a ROM streams from a USB drive can hang the
  drive until it is replugged. `--flash` stops the ROM first. When MSC0 is
  missing after a flash, replug the drive.
- When the RIA does not come back after `--flash` or `update`, reboot it by
  hand.
