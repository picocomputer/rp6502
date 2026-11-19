# Porting to the Analogue Pocket

These notes are for working on this core. Nothing here is needed to use
it — the distribution tree is described in
`src/host/pocket/dist/rp6502.txt`.

## Sources

What this core knows of the Pocket comes from these, and the files that
model the Pocket tag each behaviour with one of them:

- Analogue's developer docs,
  <https://www.analogue.co/developer/docs/overview>, above all the
  host/target commands,
  <https://www.analogue.co/developer/docs/openfpga/host-target-commands>.
- Analogue's template, `vendor/openfpga`,
  <https://github.com/open-fpga/core-template>.
- The Spacewar core, <https://github.com/spacemen3/PDP-1>, Analogue's own
  example with Sleep and Memories. Where the docs are silent it is the
  reference.
- Other community cores, named where used.
- Measurements on a Pocket. Anything else is marked as our guess.

Measured on the owner's Pocket:

- A load writes the savestate at `savestate_addr` with nothing in front.
  The Memories file on the card has a 596-byte header and a thumbnail
  around it, and neither comes to the core. This agrees with Spacewar.
- While a load request is unanswered, the Pocket serves no file command.
  One sent then gets no reply until the bridge's deadline.
- Flush, `0x0188`, gets no reply and ends at the bridge's deadline with
  result 7. Open File, Slot Write, Slot Read and Get File all reply.

## The board's memory

Analogue names the board's memory parts. This core uses only the SDRAM.
The SRAM's pins and the PSRAM's (`cram0_*`, `cram1_*`) are tied off as
Analogue's template ties off an unused part.

| | part | size | organisation | interface |
| --- | --- | --- | --- | --- |
| SDRAM | `AS4C32M16MSA-6BIN` | 64 MB | 32M×16 | 1.8 V, synchronous |

- <https://www.analogue.co/developer/docs/external-hardware>
- <https://www.alliancememory.com/datasheets/as4c32m16msa/>

**Block memory holds the 6502's 64 KB**, in 64 M10K blocks, as on the
other machines. It answers a clock after its address, well inside the
6502's shortest cycle at 8 MHz, which is six clocks of clk_sys, the
50.4 MHz system clock, so the 6502 runs at exactly the clock that was
requested.

**The SDRAM holds the soft CPU's memory**, in two blocks as on a
microcontroller. The ROM block is 256 KB of code and constants, which
the Pocket loads from `firmware.bin` and nothing writes afterwards: a
store to it is dropped. The RAM block is 64 KB of data, heap and stack.
The startup code copies the data's first values from the ROM block and
clears the rest. In front of both is one 8 KB cache in block memory,
in `cache.sv`: four ways of 128 sets, with lines of 16 bytes. A hit
answers in one clock, as block memory would. A miss holds the soft CPU
while the SDRAM controller reads the line, eight words back to back,
after writing out the line it replaces if the soft CPU stored to that
one. The rest of the block memory is XRAM, the video and the audio.

The cache's shape was measured with the firmware in simulation: the share
of the soft CPU's clocks spent waiting on it. A miss costs about 35
clocks.

| Work | 8 KB 4-way | 8 KB 2-way | 4 KB 4-way | 4 KB 2-way |
|---|---|---|---|---|
| Reset Exit to the 6502 starting | 23-34% | 24-35% | 35-39% | 35-39% |
| A program calling the API in a loop | 16% | 21% | 41% | 58% |
| The main loop idle, audio playing | 0.05% | 21% | 38% | 46% |
| Heavy file work | 12% | 20% | 40% | 45% |

Every smaller shape waits several points more, and two ways cannot hold
the idle main loop. 8 KB 2-way would use the same 9 blocks.

**The rest of the SDRAM**, the ROM image, the savestate window, the
file windows and the Get File scratch, the soft CPU reads and writes
past the cache, through the staging store. Its reads come through a
look-ahead in the SDRAM controller, which holds one line of 16 bytes.
One trip to the SDRAM fetches the line, and the reads that follow in it
are answered at once, a word at a time. Its writes go to the
controller's write client a halfword at a time, with DQM masking a byte
it does not write, after any write the host has queued. Any write, the
Pocket's or the soft CPU's, throws the line away, and a read waits for
the soft CPU's last write to land, so the firmware has no rule to
remember.

**The SDRAM's map.** The ROM slot is the first 62 MB and stays 62 MB,
because a whole ROM has to load into it. Everything else is packed into
the 2 MB above, and the savestate window is last, running to the end of
the SDRAM. Whatever the Pocket OS puts around a savestate grows into
the window's free end.

| Address | Size | Use |
|---|---|---|
| 0x00000000 | 62 MB | The ROM slot |
| 0x03E00000 | 256 KB | The soft CPU's ROM block, `firmware.bin` |
| 0x03E40000 | 64 KB | The soft CPU's RAM block |
| 0x03E50000 | 192 KB | Free, for the two blocks to grow |
| 0x03E80000 | 256 KB | Eight file windows of 32 KB |
| 0x03EC0000 | 4 KB | Get File scratch |
| 0x03EC1000 | 1276 KB | The savestate window, to 0x04000000 |

The two blocks are the soft CPU's addresses 0 to 0x4FFFF. The 512 KB
from 0x03E00000 is the soft CPU's alone and the cache's window, a power
of two, so the blocks can grow in it. The bridge drops a host write into the
two blocks while the machine runs. `stage_map_gate.py` checks every
place that states this map against the others, checks the order, and
fails if the ROM slot is anything but 62 MB.

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
and writes the savestate back into the core on wake. A load has the blob
written into the core as ordinary bridge writes and is asked for with
0x00A4. Analogue documents no order for a wake's blob, its 0x00A4 and
Reset Exit, so the core takes them in any order. On the owner's Pocket a
wake's load was still unanswered while the firmware ran, so that 0x00A4
came after Reset Exit.

The savestate is core's, made by `core/sys/sst.c` as on the emulators.
The firmware builds it, and its driver list carries
the rows. Two rows are this machine's own: `FAB_`, in `sw/fab.c`, holds
what only the bitstream keeps, and `AUD_`, in `sw/aud.c`, holds the
audio engines. The blob is about 230 KB. The size the host is given is
`SST_BYTES` in `gen/version.py`. A blob that outgrows it is refused by
`sst_save`, so the sleep tests fail in CI before it can ship. A load writes the blob at the start of
the window.

**A bitstream runs only the firmware built for it.** The register layout
version covers the registers and the memory map. It is written once, in
`gen/version.py`, which makes a package for the RTL and a header for the
firmware. The bitstream shows it at 0x80030000, and `firmware.bin`
carries its own in the word after its reset jump. `ver_check` compares
them before the firmware uses any other register, so that address never
moves. When they differ the firmware stops there and touches nothing
more. The 6502 stays in reset and the screen stays black. Raise the
version when a register the firmware uses moves, changes meaning or is
added, or when the memory map in `mmio.h`, `mem_pkg.sv` or `link.ld`
changes.

A blob from a firmware whose rows differ is refused by core's manifest,
a sum over every row's id, version and size.

### The host's commands

`pocket_sst.sv` latches 0x00A0 and 0x00A4 and answers busy until the
firmware writes its answer to `SST_CTL`. `core_bridge_cmd` holds a
request until it is acknowledged and gives the host the status lines of
that clock as the result, so the acknowledge comes one clock after the
request, when the lines already describe it.

| Request | Result |
|---|---|
| A save | busy until the firmware has made the blob, then ok; err for a refused save or a word not served in time |
| A load, the machine running | busy until the firmware has checked the blob, then ok, or err for a refused blob |
| A load, the core in reset | ok at once, and ok from then on |

**A wake's load can arrive while the Pocket holds the core in reset.**
Such a load is answered ok at once and its request is kept, so the
firmware finds it when it boots. If the firmware then refuses the blob
the answer stays ok: the host has had it.

### Saving

`sleep_task` answers the requests. `main` calls it right after
`sys_commit`, where the machine is settled as stopped or running and no
driver is partway through a pass. A save waits there until no
`pocket_file` command is in flight. A save while a file is open for
writing is answered err, because a write in progress cannot be caught
whole; `std_sst_refused` in `core/api/std.c` says so on every machine.

Otherwise the firmware writes `MACH_HOLD`. The machine stops on the
first clock its audio engines are quiet: the 6502, the VIA and the bus
stand still, and so do the audio engines' sample counters, the RIA's
vsync and mtime, so the hold costs the machine no time. The clock keeps
running, so the firmware can read every register of the machine. It runs
`sst_save` into `SST_BLOB`, the blob window in the staging store, past
the cache, lets the machine go and answers ok, or err if the save
failed. The host reads the window from the SDRAM at its own pace, with the
machine already running again.

### Loading

A load stops a running machine first, so the stop closes its files as
any stop does, and waits for the host's write queue to empty. Then the
firmware checks the blob in `SST_BLOB` with `sst_check` and answers the
request. It answers first because the rows reopen files, and the host
serves no file command until the load is answered. Then it holds the
machine and runs `sst_load`, and `apf_refresh` has the controllers sent
again, because the load put back what was held at the save.

- `FAB_` puts back mtime and the frame count, the 6502's RAM and XRAM,
  the scanline table and the RIA's registers, then RESB, and with `JAM`
  loads the 6502, the VIA, the PHI2 count and the open bus on one clock.
  mtime comes back because every deadline the firmware keeps is an
  absolute mtime reading. The time of day is the RTC reading plus
  `UPTIME`, a microsecond count that nothing stops or writes, so it
  follows real time through the hold and the load.
- `PROC` and `ASET` bring back the running program's name and its image.
  On a wake the host has loaded slot 0 again, so `fs_rom_open` takes the
  image there when Get File says the slot holds that file.
- `STD_` reopens the program's files by name, through `fs_std_reopen`.
- `AUD_` loads after `STD_`, because the reopen of PCM: empties the FIFO
  it then puts back.
- `VID_` writes the canvas and the vsync line again, builds the font
  memories for the saved code page, since none of them can be read back,
  and publishes mode 0's words for the canvas it puts back.

The firmware then lets the machine go and marks the host's slot updates
as handled so they start no program.

**A refused load stops there.** Nothing starts while a load is on its
way, because the load replaces the machine. A load with no undo leaves a
half loaded machine when a row refuses, and a fresh boot has nothing to
undo to, so a refused load says `bad savestate` and stops the machine,
which tears down whatever part of it was loaded. A launcher or an exec
that came with that part is dropped, so the machine stays stopped, as it
does after a program ends, until the user picks a ROM. A row that
refuses does so after the host has been answered ok.

### What does not survive a sleep

- The video beam is not in the blob, so a write paced by vsync can land
  up to a frame off.
- A program's clock is mtime, which stands still with the machine while
  a save or a load is made. The time of day does not, so the two drift
  apart by the hold. In simulation a save holds the machine for about
  0.44 s, and a load for about 0.11 s after it puts mtime back.
- The blob does not contain the staging store, so a reopened file starts
  with nothing in its window.

## The Core Settings menu

**The keyboard entry selects one layout.** Its value is the layout's
position in `def/keyboard.def` plus one, so zero means that the menu has
not set a layout, and the firmware then keeps the US default. The
`layout_json` ctest checks `interact.json` against the manifest, so a
layout added to the manifest and not to the menu fails a test. New
layouts go at the end of the manifest, because the setting is persisted
and a layout inserted in the middle would renumber the layouts users
have already chosen.

The entry's `defaultval` in `interact.json` is an index into the
options array, not one of the option values. The `layout_json` ctest
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

`FS:` is the microSD card, and the drive is writable. The working
directory is `/Saves/rp6502/common`, the folder for the user's data, and
no call moves it. getcwd returns `FS:/Saves/rp6502/common`, and a
relative name resolves in that folder, so `foo.txt`, `FS:foo.txt`,
`SAVE:foo.txt` and `FS:/Saves/rp6502/common/foo.txt` name the same file.
The ROMs a user picks in the Pocket's menu are in `/Assets/rp6502/common`,
and `exec` is the one call that takes a relative path under that folder
and not under the working directory, so a program runs another ROM by
its file name alone. A name that starts with a separator is absolute,
and `/` and `\` both separate.

`SAVE:` names a file in `/Saves/rp6502/common/`. `fs_save_open` joins
the name to that folder and opens it through the same code as a name on
`FS:`. The folder is the same for every ROM, so `fs_save_start` is
empty.

**The host resolves no relative name and no `.` or `..`.** A name sent
without a leading slash fails to open, so `fs_card_path` makes every
name absolute before the firmware sends it and removes each `.` and `..`
on the way. A `..` at the card root stays at the root. `fs_pool[].name`
holds that absolute path, so the reopen after a wake and the resize
before a write past the end send the same path as the open. The bench's
host model in `tests/host/pocket/test_pfile.cpp` returns 4, malformed
path, for a relative name, so a firmware change that sends one fails a
test.

A name follows the FAT rules before it is resolved, through
`path_fat_ok` in `src/core/str/path.c`, as on every other machine. A `:`
anywhere after `FS:`, or any of `"*<>?|`, a control character or DEL,
gives EINVAL. A `:` in the first part of a name without `FS:`, as in
`VCP0:x`, ends the name of a drive this machine does not have and gives
ENODEV. There is no null drive either: no ROM is installed on the
Pocket, so an `exec` of a `:name` fails.

**argv[0] is `FS:` and the path from Get File.** The host stages the ROM
image without its name, so the firmware reads the name with Get File
(`0x0190`) on the ROM slot and puts `FS:` in front of it, as in
`FS:/Assets/rp6502/common/name.rp6502`. The host writes the filename
into memory at an address given in the command. That memory is in the
SDRAM staging store, at `GETFILE_BRIDGE`, because the host cannot write
to the window that Open File reads its parameters from. The firmware
issues Get File once per staged image, in `proc_restage()`. An `exec`
does not issue it, because the program that called `exec` has already
set the arguments. `proc_exec_take` makes that argv[0] absolute before
the load, taking a relative path under `/Assets/rp6502/common`, so
argv[0] always holds the absolute path of the ROM with its drive. An
`exec` whose absolute path
is longer than 255 bytes, or whose arguments no longer fit the xstack
once the path is absolute, is refused with a message on the console.

As measured on hardware, the response holds a NUL-terminated name at
offset 0, which is where Open File's parameter struct holds its name.

The list of target commands, the commands the core sends to the host,
ends at Open File, so there is no delete, rename or mkdir. stat, unlink,
rename, mkdir, opendir, readdir and the other calls on a directory
descriptor, chmod, utime, getlabel, setlabel and getfree all return
ENOSYS. So does chdir, unless its path is the one getcwd returns, which
the load of a savestate passes. The saves that exist are found by opening names in turn,
such as `SAVE:hopper.slot0` upward. An `O_RDONLY` open fails on a missing
name without creating anything.

Because the host has no stat, an open gives EACCES for a directory only
when the path is `/Assets/rp6502/common`, `/Saves/rp6502/common` or a
folder above them, the card root included. An open of any other folder
goes to the host, and what the host returns for one is not recorded.

**The package ships both folders.** The host creates no directories, so
`src/host/pocket/dist/` has `Saves/rp6502/common/` with a `.keep` file
in it, which keeps the folder in the zip, and the build puts
`firmware.bin` in `Assets/rp6502/common/`. The card has both folders
once the core is installed.

**Much of the host behaviour in this section was measured on
hardware.** A Pocket firmware update can change it without notice. When
this section and `fstest.rp6502` disagree, the ROM is right.

**Seeking needs no host command.** Slot Read and Slot Write both include
a 32-bit offset into the file, so random access needs no cursor
protocol. A seek past the end of a file open for writing is the
exception: `fs_extend` resizes the file with Open File and then writes
zeros over the gap with Slot Write, because what a resize puts in the
bytes it adds has not been measured. The main loop stops until those
writes finish, because a driver's lseek has no pending result. A seek
past the end of a file open only for reading stops at the end.

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
that sleeps is not the same as the card having them. Analogue documents
Flush, `0x0188`, to commit them, but the Pocket does not reply to it,
so the core never sends it. Sync and close finish at once. No other
Pocket core uses Flush; the PlayStation core keeps its memory cards with
plain Slot Write.

**A sleep never lands in a command.** A save waits until no file
command is in flight, and is refused while a file is open for writing.
A wake opens each file again by name, at the position it had.

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
`fs_open_card` follows a create with a plain open to check that the file
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
drive, such as another descriptor's write,
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

**Through the debug pin.** The same bytes are sent on `dbg_tx`, pin K21,
at 115200 baud, 8N1 and 1.8 V, with no host command in the path.
`dbg_rx` is pin K22. Analogue's template calls the pair "6515D breakout
usb uart"; that is their board, and none is on this bench.

## What the card tree is made of

`src/host/pocket/dist/` holds everything the card needs except the files
that the build makes:

- `Cores/Rumbledethumps.RP6502/core.bin` comes from the Quartus build
  through `src/host/pocket/gen/rbf_r_gen.py` (byte-wise bit reversal).
  `core.json` names it; the conventional name is `bitstream.rbf_r` and
  the loader reads whichever file the manifest names.
- `Assets/rp6502/common/firmware.bin` is the soft CPU's firmware,
  `sw.bin` from the build tree's `assets` directory. It is the image of
  the ROM block: the code, the constants and the data's first values.
  The host loads it at 0x03E00000, which the soft CPU sees as address
  zero, and it may be as large as the 256 KB there. It is the only file
  the build puts in `Assets/rp6502/common/`. The fonts, the code page
  tables and the keyboard layouts are constants in it. The bitstream
  holds no glyphs, so the firmware builds the fonts on the video device
  at every boot.
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

The bitstream holds no firmware. The host writes `firmware.bin` into
the SDRAM while the core is in reset, and the core leaves reset when
the host says so and the last of those writes has left the bridge's
queue. The soft CPU's first fetch then misses the
cache and reads the firmware's first line from the SDRAM. Once the
machine runs, the bridge drops any write the host makes to the soft
CPU's memory.

`core_top` takes one parameter. `CORE_TEST_PATTERN` replaces the
picture with colour bars while leaving the machine built and still
logging on both console paths, so one build separates "is the video
path alive" from "is the machine alive".

At the ERROR log level, the default, the firmware logs nothing at boot
unless something fails, and this is among the failures it reports:

| log line | meaning |
| --- | --- |
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
the RTL, the constraints and the fitter assignments. Compiling the
firmware takes seconds and depends on the firmware's sources. Copying
the card tree is instant.

The firmware is a file on the card, `firmware.bin`, and no part of the
bitstream. A firmware change compiles, is copied into the package, and
runs no Quartus tool. On a card that already has the core, replacing
`Assets/rp6502/common/firmware.bin` is the whole update.

The timing analyzer and the Design Assistant run with the fitter.

**The project file is not a dependency of the fit.** Quartus rewrites
the `.qsf` during a compile, and the next configure writes the generated
text back, so a fit that depended on the `.qsf` would rerun after every
configure that follows a compile. The fit depends instead on the CMake
code that generates the `.qsf`.

The bring-up ROMs — `fstest`, `file`, `bigfile`, `psg`, `opl`, `probe` —
are built into `build/rtl/roms/` and are deliberately not in the
package. Copy the ones you want into `Assets/rp6502/common/` on a card
when testing.
