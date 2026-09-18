# Porting to the Analogue Pocket

These notes are for working on this core. Nothing here is needed to use
it — the distribution tree is described in
`src/host/pocket/dist/rp6502.txt`.

## The board's memory

Analogue names the board's memory parts. This core uses the SDRAM and
the SRAM, and the board's PSRAM (`cram0_*`, `cram1_*`) is not used.

| | part | size | organisation | interface |
| --- | --- | --- | --- | --- |
| SDRAM | `AS4C32M16MSA-6BIN` | 64 MB | 32M×16 | 1.8 V, synchronous |
| SRAM | `AS6C2016-55BIN` | 256 KB | 128K×16 | 3.3 V, **asynchronous**, 55 ns |

- <https://www.analogue.co/developer/docs/external-hardware>
- <https://www.alliancememory.com/datasheets/as4c32m16msa/>
- <https://www.alliancememory.com/datasheets/AS6C2016/>

**The SRAM holds the 6502's 64 KB**, because block memory is what this
design runs out of first, and 64 KB of block memory is 64 M10K blocks.

The part's 55 ns access time fits inside the 6502's shortest cycle at
8 MHz, which is six clocks of clk_sys, the 50.4 MHz system clock, or
119 ns. Unlike the SDRAM, it has no rows, no refresh and no bank
conflicts, so every access takes the same time and the 6502 runs at
exactly the clock that was requested.

Analogue's port list has no chip enable for the SRAM, so the part is
always selected and the byte enables are the only deselect.
`pocket_sram` uses the part as 64K x 8 on the low lane only, because
tAA is 55 ns whether one byte is read or two. UB# stays high, and LB#
is low only during an access, so a write happens only while WE# and LB#
are both low. Both are registered and power up high.

The controller decodes the SDRAM as 4 banks × 8192 rows × 1024 columns
× 16 bits, so a row is **2 KB**. The datasheet requires 8192 auto
refreshes every 64 ms, one every 7.8 µs, and `REFRESH_EVERY` is 390
clocks at 50.4 MHz, which is 7.74 µs.

The SDRAM's AC timings, from the `-6` mobile datasheet, at 50.4 MHz
where a clock is 19.841 ns:

| | ns | clocks | | ns | clocks |
| --- | ---: | ---: | --- | ---: | ---: |
| tRCD | 18 | 1 | tRAS min | 48 | 3 |
| tRP | 18 | 1 | tRAS max | 100 µs | 5040 |
| tRC | 60 | **4** | tRFC | 80 | **5** |
| tDPL | 2 tCK | 2 | tXSR | 80 | 5 |

Two of those limits and the refresh interval sit close to a whole
number of clocks. tRC at 3 clocks is 59.5 ns and misses 60 by half a
nanosecond, and tRFC at 4 clocks is 79.4 ns and misses 80 by 0.6 ns, so
each takes one clock more. The refresh interval is a **ceiling that
rounds down**: 393 clocks fits inside 7812.5 ns and 394 does not.

The extended mode register holds Partial Array Self Refresh (PASR) and
driver strength, and it powers up in a state the datasheet does not
name. PASR sets which banks are refreshed during self refresh, while an
auto refresh covers the whole array regardless of it. The controller
puts the SDRAM into self refresh when it is idle, so it writes the
register during initialization with PASR selecting all four banks.

## Suspend

Sleeping on openFPGA means producing a savestate, because wake
reconfigures the FPGA and the core starts again from its bitstream. At
sleep the host sends command 0x00A0 to create a savestate, powers down,
and writes the savestate back into the core on wake. The boot
documentation fixes the order in both directions: a create signals
completion before the data is copied out, and a load has the blob
written into the core as ordinary bridge writes before 0x00A4 arrives.

The blob is 317 KB, and the savestate engine, `sst_engine.sv`, makes it
in logic rather than in firmware.

### The architecture: one clock gate

**The machine's clock stops at its source.** One `altclkctrl` in
`core_top.sv` gates clk_sys to make clk_mach, the machine clock, and it
registers its enable on the falling edge of clk_sys so no high phase of
clk_mach is cut short. The 6502, the VIA, the RIA's control logic,
video and audio all run on clk_mach and stop wherever they are. Every
register on clk_mach misses the same edge and resumes on the same edge,
so no logic in the machine needs a clock enable for a savestate.

Three parts keep their clocks. The SDRAM staging store, which holds
what the host writes into the data slots, stays on clk_sys, so its
controller keeps it refreshed. The engine and the memory arrays stay on
clk_sys too, and the engine reads and writes the arrays through a
separate port. The arrays sit idle because the logic driving their
machine-side inputs is stopped, and the engine masks their frozen write
enables with `arr_own` for the whole savestate, because a write enable
that froze high would otherwise repeat its stale write every cycle. The
soft CPU is not clock-gated at all. Its clock, clk_rv, keeps running,
and the engine halts it at its debug port and spills its registers by
injecting `csrw dmdata0, xN` and then `ebreak`, which marks the end of
each injected instruction.

A machine signal consumed by logic on an ungated clock is a **level**,
not an event. When clk_mach stops, the signal freezes wherever it was,
and a consumer that takes it once per cycle acts on it on every cycle
of the savestate. Each such consumer on this board is therefore clocked
by clk_mach: the pixel queue in `pocket_video.sv`, the sample queue in
`pocket_i2s.sv`, and the console queues in `pocket_dbg.sv` and
`pocket_dbglog.sv`. Where a mask is needed instead, it takes the form
of `arr_own`. `arr_own` rises only once the gate has closed, so it
never masks an access the machine makes before then, and it falls as
soon as the stop request drops, which is several cycles of clk_74a, the
74.25 MHz bridge clock, before the gate opens again. `pocket_sram`'s
port A mask, `mach_gated` in `pocket_core.sv`, has the same form,
because a mask that waited for the synchronized clock enable would stay
up for two clk_sys cycles after clk_mach returns, and the 6502 could
take a PHI2 enable inside them.

A save runs in this order. The engine halts the soft CPU, spills its
registers and stops the machine clock. It then checksums the whole blob
in one pass before it signals completion, because the host may read
words in any order and more than once. It serves each word as the host
reads it, and a word that is not ready by the host's next read sets the
save's error status. Serving the last word ends the save, because
completion was signalled before the copy began and no host command
marks the copy's end. The engine then restarts the machine clock and
asserts the soft CPU's resume request, `dbg_req_resume`, since Hazard3
stays halted when its halt request drops.

### Restoring

The engine checks the whole staged blob before it writes any of it,
because a blob found bad partway through writing would leave the
machine half overwritten. Then, with the machine still stopped, every
array is written back through the engine's ports, and the flop state
(6502, VIA, RIA window flops, resb, the 6502's clock rate, the
microsecond counter) is held in the engine until the steps below write
it back.

Waking the machine takes these steps in order:

1. The engine's reset hold, `hold_res`, rises while the machine is
   still stopped. It drives the asynchronous resets of the 6502 and the
   VIA, so it needs no clock.
2. The machine clock returns with the 6502 already in reset, so the
   6502 takes no cycle that could write over the restored memory.
3. The soft CPU's microsecond counter, mtime, is written back on a
   separate level held through the register injection in step 4,
   because the soft CPU resumes before the jam in step 5. mtime is in
   the blob's state page because every deadline the firmware keeps is
   an absolute mtime reading in the TCM, which the blob contains, so
   without it each deadline would be off by the difference between the
   counter's value at the save and its value at the restore.
4. The soft CPU's registers are written back through the debug port,
   and the core resumes at the instruction where the save halted it,
   running the blob's firmware on the blob's stack. The firmware reads
   `SST_RESTORED` set and puts back the state the blob does not
   contain: it rebuilds the font store from the font asset and the code
   page, replays the active audio engine's register block because the
   engines take their registers only from XRAM writes, rewrites the
   canvas, the vsync line and the terminal's window from its own
   copies, marks every open file to be checked and, if needed,
   reopened by its saved name, derives the wall clock again from the
   host's RTC, and resynchronizes the restage triggers so the slot
   updates of the wake do not start a new program.
5. The firmware clears `SST_RESTORED`, and two jam cycles follow, in
   which the engine writes the rest of the held flop state into the
   flops. The first writes `resb`, a flop with no reset, and drops
   `hold_res`. In the second, every 6502, VIA and RIA flop takes its
   saved value with the asynchronous resets already released, since an
   asynchronous reset takes priority over the jam. Two clk_sys cycles
   are also one period of the soft CPU's half-rate clock, so the soft
   CPU's register for the 6502's clock rate takes the jam too.

The 6502 then continues from the cycle at which the save stopped it.

### The console queue

The RIA holds up to sixteen bytes of console output that the 6502 has
written and the soft CPU has not read yet. Reading word 16 of the regs
window removes a byte from that queue, so `REGS_HOLE` leaves that word
out of the blob, since a save that read it would drop a character every
time a state was made.

The queue is saved through words 20 to 24 of the regs window instead,
which can be read without side effects: word 20 holds the read and
write positions and the count, and words 21 to 24 hold the sixteen
bytes. The bytes are an array on clk_sys and are written back through
the window with the xstack, and the positions and the count are flops
on clk_mach that the jam restores.

### What does not survive a sleep

- The blob does not contain the PSG's envelopes, phase and noise or the
  OPL's internal generators, so the engines start them again from the
  replayed registers. A held note comes back because the replay keys
  the active engine again: writing `0xB0-0xB8` keys an OPL voice on,
  and the PSG takes the gate bits in the replayed block while the
  firmware holds `AUD_PSG_REPLAY` set. The PSG otherwise takes a gate
  only from a 6502 write, so without `AUD_PSG_REPLAY` a voice that was
  sounding at the save would stay silent for the rest of the program.
- The blob does not contain the host's slot bindings or the staging
  store, so `fs_restore` marks every open file stale and drops its
  cached bytes. Analogue does not document whether a slot keeps a
  binding made at runtime with Open File (`0x0192`) across a sleep, so
  on the file's next use `fs_rebind` reads the slot's binding with Get
  File (`0x0190`) and opens the file again by its saved name only when
  the slot no longer holds it.

### The picture across a freeze

The scaler is in the Pocket's other FPGA and keeps running while the
machine is stopped, so the reader in `pocket_video.sv` keeps its
raster, its vs, its hs and its de running the whole way through, and
only the pixels go black. The frames the scaler receives keep the same
shape throughout.

The lock between the two rasters does not survive a freeze. The
rasters are phase-locked with the writer at least `X_DE0`, eight
pixels, ahead, and a freeze stops the writer mid-frame and restarts it
mid-frame while the reader keeps counting. The lock is taken again from
a tag bit that the writer pushes into the queue with the frame's first
pixel, so the tag arrives at the reader with its pixel and not after
it. The reader discards pixels up to the tag and locks on it. Relocking
restarts the reader's raster at its first pixel and pulses vs, which
cuts short the frame the reader was in, so each resume produces one
short frame.

## The Core Settings menu

**The keyboard entry selects one layout.** Its value is the layout's
position in `def/keyboard.def` plus one, so zero means that the menu has
not set a layout, and the firmware then keeps the US default. The
`kbdlay_json` ctest checks `interact.json` against the manifest, so a
layout added to the manifest and not to the menu fails a test. New
layouts go at the end of the manifest, because the setting is persisted
and a layout inserted in the middle would renumber the layouts users
have already chosen.

The entry's `defaultval` in `interact.json` is an index into the
options array, not one of the option values. The `kbdlay_json` ctest
checks that the option `defaultval` selects is US, which is the layout
`keymap.c` falls back to, so the two defaults agree.

**The time zone is three entries, not one.** The host sends the local
time with no time zone, so the offset has to be set by hand, and it
cannot be one entry. A list holds at most sixteen options and the
offset spans twenty-seven whole hours, so it is a side (east or west),
an hour (0 to 14) and a quarter hour. Each entry writes a separate
register, and `set_tz_minutes` in `mmio.h` combines the three into
minutes east of UTC.

**`input.json` lists no controllers.** The core passes every controller
slot to the firmware as a HID report, buttons and axes, so there is no
fixed button mapping to list.

**A hot reload goes through the first slot, whichever slot the request
names.** Picking a new file from this menu makes the host write the
image through the first slot record in `data.json`, so the ROM is the
first slot, at address zero. The ROM slot is not a deferload slot,
and the host sends `0x008A` only for one, so a hot reload of the ROM is
a request write, the image, its data table entry and a second `0x008F`,
access all complete. When this `0x008F` arrives while the core is
running, the new size is posted to the firmware, which stops the
program and loads the new image.

## The dock, as HID

The core receives three registers, key, joy and trig, for each of APF's
four controller slots. The top nibble of a slot's key word gives the
device in it: none, the Pocket's own buttons, a docked controller with
or without analog sticks, the docked keyboard, or the docked mouse.

The firmware has no input drivers of its own for the dock. `apf.c`
mounts each slot with connection structs written by hand, which is the
form `src/core/hid/parse.c` produces from a HID report descriptor, and
passes the registers to the HID drivers as the report those structs
describe. `src/host/pico/ria/usb/xin.c` does the same for XInput, which
has no report descriptor. The drivers in `src/core/hid` then provide the
layouts, dead keys, key repeat and terminal escape sequences. The mouse
is mounted as a tablet as well as a mouse, and the d-pad is reported as
a d-pad rather than as the left stick.

The connection structs in `apf.c` hold the whole mapping, so a wrong one
still builds and boots, and the fault appears only when a controller is
used. `tests/host/pocket/test_apf.c` feeds register values through
`apf.c` into the real HID drivers and checks the XRAM records they
produce.

There is no monitor, so `sys_break` in `main.c` returns false and
Ctrl-Alt-Del is passed to the program as an ordinary keystroke. Alt-F4
returns to the launcher when one is registered and the running program
is not the launcher. Otherwise `sys_break_to_launcher` returns false and
Alt-F4 is passed to the program too.

## The host's filesystem

`FS:` is the microSD card, and the drive is writable. The firmware
strips `FS:` from a name, and a name without a leading slash is
relative, so the firmware prefixes it with `/Saves/rp6502/common/`. A
name that starts with a slash is absolute and gets no prefix. `foo.txt`
and `FS:foo.txt` name the same file, and
`FS:/Assets/rp6502/common/foo.txt` names a file in the package's folder
under Assets.

`FS:` is a name a program can use for the drive. The firmware does not
add it to the paths it returns, such as the one getcwd returns and
argv[0], because a path on this card has no device prefix, just as a
POSIX path has none.

**The host does not resolve relative names.** A name sent without a
leading slash opens nothing, so every name the firmware sends is
absolute. The bench's host model in
`tests/host/pocket/test_pfile.cpp` returns 4, malformed path, for a
relative name, so a firmware change that sends one fails a test.

Since the host keeps no working directory, getcwd is implemented in the
firmware. It returns `/Saves/rp6502/common`, which is where relative
names resolve, so appending a separator and a name opens the same file
as the bare name. The path has no trailing separator, so a program that
appends one does not double it. chdir fails for every path, including
that one.

**Relative names resolve under two different folders.** A machine with
no working directory still has to define what a bare name means, and
the two places a bare name comes from need different folders:

| a relative name naming    | resolves under           |
| ------------------------- | ------------------------ |
| a file, through `std` open | `/Saves/rp6502/common/`  |
| a program, through argv   | `/Assets/rp6502/common/` |

Saved files go in Saves. Programs are in Assets, because the ROM a user
picks in the Pocket's menu comes from that folder. `fs_try_open` takes
the folder to resolve against as an argument, so `exec` resolves a
relative program name under Assets and every other open resolves under
Saves.

**argv[0] keeps the prefix the host gives it.** It arrives as an
absolute path, `/Assets/rp6502/common/name.rp6502`, so `open(argv[0])`
opens the program. Stripped to a bare name, it would resolve under
Saves, where the program is not.

**argv[0] comes from Get File.** The host stages the ROM image without
its name, so the firmware reads the name with Get File (`0x0190`) on
the ROM slot. The host writes the filename into memory at an address
given in the command. That memory is in the SDRAM staging store, at
`GETFILE_BRIDGE`, because the host cannot write to the window that Open
File reads its parameters from. The firmware issues Get File once per
staged image, in `proc_restage()`. An `exec` does not issue it, because
the program that called `exec` has already set the arguments.

As measured on hardware, the response holds a NUL-terminated name at
offset 0, which is where Open File's parameter struct holds its name.

The list of target commands, the commands the core sends to the host,
ends at Open File, so there is no delete, rename or mkdir, and those
calls return ENOSYS. opendir returns ENOSYS too, so a program finds its
files by opening names in turn, such as `save00.dat` upward. An
`O_RDONLY` open fails on a missing name without creating anything.

**The drive's folder ships in the package.** The host creates no
directories, so `src/host/pocket/dist/` has `Saves/rp6502/common/` with
a `.keep` file in it, which keeps the folder in the zip, and the card
has the folder once the core is installed.

**Much of the host behaviour in this section was measured on
hardware.** A Pocket firmware update can change it without notice. When
this section and `fstest.rp6502` disagree, the ROM is right.

**Seeking needs no host command.** Slot Read and Slot Write both include
a 32-bit offset into the file, so random access needs no cursor
protocol.

**Creating a file takes both flag bits.** With bit 0, create, on its
own, Open File returns 1, created and opened, and makes no file. The
file is made only when bit 1, resize, is set as well. Both bits on an
existing file resize it to the size given, and a create gives a size of
zero, so the firmware first opens the name with no flags to find out
whether the file exists. Open File has no flag for exclusive creation,
so the same open serves `O_EXCL`.

**Open File's parameter struct holds its integers as words and its path
as bytes.** With `bridge_endian_little` clear, byte zero of the path is
in bits 31:24 of its word, but the host reads the flags at 0x100 and the
size at 0x104 as whole bridge words. Flags of 3 packed like the path,
low byte first, arrive as `0x03000000`, which clears both documented
bits and sets two reserved ones. The host then opens the file without
creating or resizing it, so the open succeeds on an existing file and
returns 3, not found, on a missing one.

**Only a shrink shows that a resize happened**, because a Slot Write
past the end of a file produces the same length whether or not the
resize took effect.

**Open File has two success codes**, 0 for opened and 1 for created and
opened, while the other commands succeed only with 0. Among the
failures, 3 is not found and 4 is malformed path.

**A completed Slot Write does not mean the card has the data.** Slot
Write returns once the host has taken the bytes, which on a handheld
that sleeps is not the same as the card having them. Flush, `0x0188`,
would commit them, and the Pocket does not reply to it. The bridge
override in `vendor/openfpga_rp6502` gives up on a data slot command
after about 0.9 s without a reply, so a flush costs one deadline and not
the session. A flush is sent on every sync and on every close of a file
open for writing until one gets no reply. After that no flush is sent
for the rest of the session, so a write is only as durable as the
host's acceptance of the bytes.

**The bridge's deadline is the shorter of the two.** The bridge and
`pocket_file` both time out a data slot command, the bridge after 2^26
cycles of clk_74a, about 0.9 s, and `pocket_file` after 2^27, about
1.8 s. The bridge is the side left waiting on a silent host, and only
its own timeout returns it to idle. If `pocket_file` gave up first, the
bridge would still be waiting when the next command started.
`pocket_file` takes a command as accepted when `target_dataslot_done`
falls, so the next command would find `done` already low, take the
abandoned command's late result as its own, and then run at the host
with nothing waiting for its result. With the bridge's deadline the
shorter, a command the host ignores comes back as result 7 and not as a
`pocket_file` timeout.

**A create into a missing folder fails without an error.** No data slot
command creates a directory, and the host does not create the folders
in a path. An Open File with both flag bits for a file in a folder that
does not exist returns 1, created and opened, and makes no file, so
`fs_std_open` follows a create with a plain open to check that the file
exists.

## The host interface

APF packs the bytes of a controller register most significant first, so
the keyboard's first scan code is in `joy[31:24]`. The keyboard's
modifier field and the mouse's sixteen-bit fields are little endian
within that packing, so each field arrives with its low byte in its high
half, and `apf.c` reads them that way.

`fstest.rp6502` exercises the whole drive in one boot and prints its own
result, a pass count and the number of each check that failed, in the
simulator as well as on the card.

### Get File on an unbound slot

Get File returns 0 for a bound slot and also for a slot that is defined
and bound to nothing, which is every one of the eight file slots until a
program opens one. The host writes the whole 256-byte response struct on
every Get File, and the struct holds the path for a bound slot and starts
with a NUL for an unbound one, so `fs_getfile` treats an empty name as
an unbound slot.

`pocket_file` sets the status bit `FILE_ST_WROTE` when the host writes
into the staging store while a Get File is outstanding. Since the host
writes the struct for a bound slot and for an unbound one, the bit is set
in both cases and does not distinguish them.

### One file command at a time

`pocket_file` runs one file command at a time and reports its status in
one register. `fs.c` issues each command under a worker id, one per
descriptor and one for the firmware's own commands. While one worker's
command is in flight, other work in the same main loop can use the
drive, such as a rebind after a restore or another descriptor's write,
and a read that took another worker's status would copy a window that
was not filled for it. `fs.c` records the worker whose command is in
flight and keeps a finished command's status for that worker rather than
dropping it, so a blocking command can wait for another worker's command
to finish instead of deadlocking against it.

## Reading the console

The soft CPU's log holds the firmware's own lines, such as
`ERROR rom: bad image`, and never a program's output, which goes only to
the screen. The log leaves the core by two paths, and every bitstream
has both. The log level is set when the tree is configured, by
`RP6502_LOG_LEVEL`. This directory's `CMakeLists.txt` sets it to ERROR
by default, and with `-DRP6502_LOG_LEVEL=DEBUG` the firmware logs every
line.

**Through the Pocket.** The log's bytes are sent to the host in target
command 0x0152, four bytes in each command's 32-bit event id with the
first byte in the top eight bits, so the id reads left to right as
ASCII: `4552524F` is `ERRO`. A word of fewer than four bytes at the end
of a burst is left-justified and zero-filled. Each command is a round
trip through the host, so the log lags, and bytes are dropped when the
firmware logs faster than the host completes the commands.

**Through the debug pin.** The same bytes are sent on `dbg_tx` at 115200
baud, 8N1 and 1.8 V, to the USB UART on the 6515D breakout board, with
no host command in the path.

## What the card tree is made of

`src/host/pocket/dist/` holds everything the card needs except the files
that the build makes:

- `Cores/Rumbledethumps.RP6502/core.bin` comes from the Quartus build
  through `src/core/gen/rbf_r_gen.py` (byte-wise bit reversal).
  `core.json` names it; the conventional name is `bitstream.rbf_r` and
  the loader reads whichever file the manifest names.
- `Assets/rp6502/common/fonts.bin` is the glyph image, generated by
  `src/core/gen/vid_font_gen.py --emit-bin` from `src/core/term/font.c`
  into the build tree's `assets` directory. It holds every face and all
  seventeen code pages. It loads near the top of the SDRAM, above the
  file slots' windows — slot addresses rise with slot id, and the ROM,
  slot 0, starts at address zero and can fill everything below its
  `size_maximum` of 0x03F00000. The bitstream holds no glyphs, so the
  firmware copies them from this image to the video device at every
  boot.
- `Assets/rp6502/common/oemcp.bin` and
  `Assets/rp6502/common/keyboard.bin` are made the same way, by
  `src/core/gen/oem_table_gen.py` and
  `src/core/gen/keyboard_layout_gen.py`. They are the code page
  conversion tables and the keyboard layouts.
- `Cores/Rumbledethumps.RP6502/icon.bin` and
  `Platforms/_images/rp6502.bin` are here already, made by
  `src/host/pocket/gen/image_gen.py`. Both use one format: two bytes a
  pixel, with the brightness in the first byte and zero in the second,
  the raster stored rotated a quarter turn counter-clockwise, and no
  header. Only the size differs, 36x36 and 521x165, so each file is
  exactly width × height × 2 bytes long. The build does not run the
  script, so it is run again by hand when the artwork changes and what it
  makes is committed.

## First bring-up

`core_top` takes two parameters. `TCM_INIT_FILE` names the soft CPU's
firmware image, the four byte-lane files that the build writes with
`src/host/pocket/gen/rv_tcm_gen.py`. Without an image the TCM holds
zeros and the soft CPU fetches them. `CORE_TEST_PATTERN` replaces the
picture with colour bars while leaving the machine built and still
logging on both console paths, so one build separates "is the video
path alive" from "is the machine alive".

At the ERROR log level, the default, the firmware logs nothing at boot
unless something fails, and these are among the failures it reports:

| log line | meaning |
| --- | --- |
| `ERROR oem: no tables` | the code page slot did not stage |
| `ERROR keyboard: no layouts` | the layout slot did not stage; keys that type a character type nothing |
| `ERROR rom: bad image` | the ROM staged but did not load |

From this directory, `cmake --preset release` then
`cmake --build --preset release` assembles the card tree into
`build/pocket/package`. Zip the contents of `build/pocket/package` with
its directories at the archive root. The `pocket-bitstream` target stops
one step earlier, at `build/pocket/bitstream/core.bin`.

That tree needs Quartus, `gcc-riscv64-unknown-elf` and
`picolibc-riscv64-unknown-elf`, and it needs neither Verilator nor the
test suite.

## What each change costs

The default target is the only one needed, because the three steps this
build runs differ greatly in cost and CMake reruns only the steps whose
inputs an edit changed.

Synthesis, placement and routing take about nine minutes and depend on
the RTL, the constraints and the fitter assignments. Putting firmware
into the bitstream takes about half a minute and depends on the
firmware's sources. Copying the card tree is instant.

The firmware is cheap because it is not logic. It is the initial
contents of the TCM's four byte-wide arrays, so a new image places
nothing, routes nothing and moves no timing arc, and the fit already on
disk is still the fit that comes out. Quartus keeps a MIF of each array
under `db/`, generated from `soc`'s `$readmemh` while mapping, and those
are what `quartus_cdb --update_mif` reads back rather than the lane
files they came from. `rv_mif_gen.py` rewrites the four,
`--update_mif` takes them into the database, and the assembler makes a
programming file out of the placement that was already there.

The timing analyzer and the Design Assistant therefore run with the
fitter and not when only the firmware changes, since rerunning them
would re-measure a fit that has not changed.

**The project file is not a dependency of the fit.** Quartus rewrites
the `.qsf` during a compile, and the next configure writes the generated
text back, so a fit that depended on the `.qsf` would rerun after every
configure that follows a compile. The fit depends instead on the CMake
code that generates the `.qsf`.

The bring-up ROMs — `fstest`, `file`, `bigfile`, `psg`, `opl`, `probe` —
are built into `build/rtl/roms/` and are deliberately not in the
package. Copy the ones you want into `Assets/rp6502/common/` on a card
when testing.
