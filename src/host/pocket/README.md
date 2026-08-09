# Porting to the Analogue Pocket

Notes for working on this core. Nothing here is needed to use it — the
distribution tree is described in `src/dist/pocket/rp6502.txt`.

## The board's memory

Analogue names the parts, which for a long time this file assumed they
never would. There are three, and for most of this port's life we used
one of them.

| | part | size | organisation | interface |
| --- | --- | --- | --- | --- |
| SDRAM | `AS4C32M16MSA-6BIN` | 64 MB | 32M×16 | 1.8 V, synchronous, 166 MHz |
| PSRAM | `AS1C8M16PL-70BIN` | 16 MB | 8M×16 | 1.8 V, synchronous burst, 133 MHz |
| SRAM | `AS6C2016-55BIN` | 256 KB | 128K×16 | 3.3 V, **asynchronous**, 55 ns |

- <https://www.analogue.co/developer/docs/external-hardware>
- <https://www.alliancememory.com/datasheets/as4c32m16msa/>
- <https://www.alliancememory.com/datasheets/AS6C2016/>

Analogue's own notes are worth quoting, because each one is a design
constraint rather than a description. On the SDRAM: *"best accessed in
bursts since every access requires activation and precharge
overhead."* On the PSRAM: *"Be careful to never assert both chip enable
pins (CE0#, CE1#) to avoid bus contention"* — it is two dies sharing
every signal but those. On the SRAM: *"Available to user cores.
Latency is marginally faster than PSRAM but possible bandwidth is
lower."*

**The SRAM holds the 6502's 64 KB**, and it was tied off dead in
`core_top.sv` for most of this port's life while block memory did that
job. Block memory is what this design runs out of first: the fit sat at
275 of 308 M10K with `sram64k`, `xram64k` and the firmware's TCM taking
64 blocks each. Moving the 6502 off-chip freed exactly the 64 that
doubling the TCM to 96 KB cost — 243 of 308 now, and the firmware went
from 5,048 bytes of stack and heap to 39,584.

An asynchronous part answering in 55 ns fits a bus that allows 119 ns
at 8 MHz, and unlike the SDRAM it answers in 55 ns *every* time: no
rows, no refresh, no bank conflicts, nothing a program can do to make
it slower. For a machine whose contract is that the clock you asked for
is the clock you get, deterministic beats fast-on-average.

Note what is *not* in that port list: there is no chip enable, so the
board holds the part selected and `OE#`/`WE#` are the whole protocol —
which makes `WE#` the only interlock against a stray write, and it is
registered with a known power-on state for that reason. The byte
enables are there and constant: `pocket_sram` uses 64K x 8, the low
lane only, because a 16-bit read buys nothing when tAA is 55 ns for one
byte or for two. The PSRAM (`cram0_*`, `cram1_*`) is still untouched.

The SDRAM's own geometry is worth writing down because the controller
decodes it: 4 banks × 8192 rows × 1024 columns × 16 bits, which makes a
row **2 KB** and puts the 6502's whole address space inside 32 rows.
Refresh is 8192 cycles per 64 ms, so 7.8 µs a cycle; `REFRESH_EVERY` is
390 clocks at 50.4 MHz, which is 7.74 µs.

Its AC timings, from the `-6` mobile datasheet itself, at 50.4 MHz
where a clock is 19.841 ns:

| | ns | clocks | | ns | clocks |
| --- | ---: | ---: | --- | ---: | ---: |
| tRCD | 18 | 1 | tRAS min | 48 | 3 |
| tRP | 18 | 1 | tRAS max | 100 µs | 5040 |
| tRC | 60 | **4** | tRFC | 80 | **5** |
| tDPL | 2 tCK | 2 | tXSR | 80 | 5 |

Three of those round the wrong way if you are careless. tRC at 3 clocks
is 59.5 ns and misses 60 by half a nanosecond; tRFC at 4 misses 80 by
the same; and the refresh interval is a **ceiling that rounds down** —
393 clocks fits inside 7812.5 ns and 394 does not. CL2 is rated to
83 MHz, so it is not close to marginal here; CL1 would be illegal,
rated to 50 MHz against our 50.4.

The extended mode register matters more than it looks. It holds
Partial Array Self Refresh and driver strength, powers up in a state
the datasheet declines to name, and this controller did not write it
for a long time. That was survivable only while the store never slept:
PASR governs self refresh alone, and an auto refresh covers the whole
array whatever it says.

## Suspend

`core.json` says `"sleep_supported": false`, and that is a decision
rather than an omission.

Sleeping on openFPGA means producing a savestate. At sleep the host
asks 0x00A0 for a blob, powers down, and hands it back on wake; a core
that cannot make one is powered down without it. This core cannot, so
for a while it declined the blob and let the Pocket sleep anyway,
waking into a cold boot on the theory that a machine with a power
switch wakes to a reset.

Hardware disagreed, and the owner said so plainly: the ROM restarted
instead of continuing. So the machine was asked what survives. A
marker was written into the firmware's own memory — a `.noinit`
section the bitstream would overwrite and `crt0` leaves alone —
alongside the microsecond counter, which anything at all resets, and
the pair was printed at every boot. Across two sleep cycles the marker
was gone and the counter back at nothing, every time.

**Wake reconfigures the part.** SRAM, XRAM, TCM and every register come
back out of the bitstream. Nothing the firmware can do continues a
program through that; only a genuine savestate could, and that means
marshalling something like 200 KB of machine state through the bridge
with 153 ALMs spare — a feature that costs another feature.

Since a sleep that silently restarts the user's program is worse than
a Pocket that simply does not offer sleep for this core, the core stops
claiming it. The savestate inputs in `core_top.sv` stay driven and
denying, so a host that asks anyway still gets a real answer rather
than a floating one.

## The Core Settings menu

Five things live there and one used to that should not.

**The keyboard is two entries.** "Keyboard" is required and defaults to
US; "Keyboard, alt" adds a second and offers None. Together they are
the layout list the machine's own keyboard driver takes, so GUI+Space
cycles between them exactly as it does on a RIA. Both name a layout by
its position in `def/kbd.def` plus one, which leaves zero meaning a
menu that has said nothing — the alternate's own zero is its None. A
ctest reads `interact.json` against the manifest, so adding a layout
and forgetting the menu fails the build rather than shipping a list
that names the wrong one. Append new layouts at the end of the
manifest: the host persists this setting by its value, and inserting
one in the middle would renumber what a user already chose.

**The time zone is three entries, not one.** The Pocket knows nothing
about time zones, so the offset has to be set by hand — and it cannot
be one control. A list holds at most sixteen options and the offset
spans twenty-seven whole hours, so it is a side (east or west), an
hour (0 to 14) and a quarter hour. A slider was tried first and the
owner reported it garbled: its numbers ran to 840. The three write
three separate registers because APF can mask several elements into
one word only by reading it back first, and these registers are
write-only. `set_tz_minutes` in `mmio.h` is the single place they are
put back together.

**The Controls submenu is gone.** APF builds it from `input.json`,
which claimed the Pocket's buttons were Enter, Escape, Space and so
on. This core does no such thing: it hands every controller slot to
the firmware as a HID report, buttons and axes, exactly as the
machine's own HID API expects. The file is now an empty controller
list, since the menu was documenting a mapping that does not exist.

**The last ROM no longer relaunches itself.** Slot 0 had bit 9 of its
parameter bitmap set — "persist browsed filename" — which makes APF
remember the file and reload it on every core load, overriding the
browser. Cleared, so the core asks each time.

**A hot reload goes through the first slot, whatever slot was asked.**
Measured, with the ROM temporarily at id 10: picking a new file from
this menu made the host write the image through the *first* slot
record — the debug log says `Load 0x00000000 ... slot name [File 0]` —
while the request write and the table entry both named id 10
correctly. Its own docs call slot 0 "the primary data slot", and the
reload path plainly assumes the reloadable slot is that one. No
`0x008A` is sent for a non-deferload slot either, documentation
notwithstanding; the reload is a request write, the image, the table
entry, and a second access-all-complete. So the ROM is the first slot
at address zero, and the firmware treats the size that completion
posts mid-run as the announcement it is.

## The dock, as HID

APF polls its four controller slots and hands the core three registers
for each one. What a slot holds is the top nibble of its key word:
nothing, the Pocket's own buttons, a docked controller with or without
analog, the docked keyboard, or the docked mouse. Analogue assigns the
keyboard and the mouse to particular slots today and says the
assignment is the framework's to make, so the firmware trusts the
nibble and not the slot number. All four cross the clock domain
identically and none of them is named for a device.

The firmware does not have its own drivers for any of this. `apf.c`
mounts each slot with a HID report descriptor written by hand and
hands the registers over as the report that descriptor describes,
which is what `ria/usb/xin.c` does for XInput — a device with no
descriptor of its own. `ria/hid` then does the rest: layouts, dead
keys, key repeat, the escape sequences a terminal expects, the mouse
landing in the tablet driver as well as the mouse one, four players in
slot order, and a d-pad that is a d-pad rather than a left stick
pretending.

Three descriptors carry the whole mapping, so a wrong one would build
clean and boot clean and be wrong only under a hand on a controller.
`tests/hid/test_apf.c` is what stands in for that hand: registers in,
XRAM records out, against the real drivers.

Two things a RIA has are not here. There is no monitor, so
Ctrl-Alt-Del is an ordinary key — `main_break` says no and the
keystroke falls through. Alt-F4 works when a launcher is registered,
because then there is somewhere to go; from inside the launcher, or
with none registered, it says no too.

## The host's filesystem

`MSC0:` is the card, and the drive writes. The drive prefix is
stripped and the slash after it decides everything: a name that
follows the colon directly is relative and the firmware spells it out
against `/Saves/rp6502/common/`, while a name that starts with a slash
is already absolute and travels untouched. `foo.txt` and
`MSC0:foo.txt` are the same saved game;
`MSC0:/Assets/rp6502/common/foo.txt` reaches the package's own folder,
which is writable. A program's plain `open("game.save", ...)` lands in
the same place on every platform.

**The host resolves nothing.** It looked for a while as though it kept
a working directory at `/Saves/rp6502/common/`, and one run settled
it: `004.bin` spelled in full appeared on the card and `000.bin`
spelled bare never did, same folder, same boot. Every name reaches the
host absolute or it does not arrive. The bench refuses a relative path
outright so the firmware cannot quietly go back to hoping.

getcwd is therefore entirely ours: it answers
`MSC0:/Saves/rp6502/common/`, which is where relative names go, so
appending a name to it opens the same file the bare name does. chdir
errors whatever it names, even that directory.
**Two pinned folders, and they are not the same one.** This is the
platform hack, written down here because nothing about it is
guessable from the API. A machine with no working directory still has
to say what a bare name means, and the two places a bare name comes
from want different answers:

| a relative name naming    | resolves under           |
| ------------------------- | ------------------------ |
| a file, through `std` open | `/Saves/rp6502/common/`  |
| a program, through argv   | `/Assets/rp6502/common/` |

Saved games belong in Saves; programs are in Assets, because that is
where the Pocket's menu browses and where the host bound the ROM slot
from.
Both rows are live: the open path takes the root it should resolve
against, so `exec` spells a bare program name out under Assets and
everything else under Saves.

**argv[0] keeps the prefix the host gave it.** It arrives absolute,
`/Assets/rp6502/common/name.rp6502`, and is passed through untouched.
Stripping it to a bare name was tried and is wrong twice: an absolute
path is exactly what the drive does not re-resolve, so
`open(argv[0])` finds the program, and a bare argv[0] would already
be relative to something before `exec` ever got to apply the rule
above.

**argv[0] is asked for, not known.** The core is handed a staged
image and never told what it was called. Get File (`0x0190`) on the
ROM slot is the only way to learn the name, and it is the one data
slot command whose answer is more than a result code: the host writes
the filename into memory the core nominates. That has to be the SDRAM
staging store, since the bridge writes nowhere else — the outbound
window Open File uses is write-only from the host's side — so the
answer lands in a dedicated scratch above the assets at the top of the
store. The firmware asks once per staged image, in `pro_restage()`. An
`exec` does not ask — the outgoing program has already said what the
arguments are.

Analogue documents the command and not the shape of its response
struct. We read a NUL-terminated name at offset 0, which is where
Open File's *parameter* struct carries one. That began as a guess with
a good reason behind it; hardware settled it, and argv[0] comes back
correct on a Pocket.

There is no delete, rename or mkdir: the target command list ends at
Open File, and those calls answer ENOSYS. Existence is probed with a
plain `O_RDONLY` open, which fails on a missing name without creating
anything; trying `save00.dat` upward is the directory listing this
host will ever have.

**The drive's folder ships in the package.** The host creates no
directories, so `src/dist/pocket/` carries `Saves/rp6502/common/` with a readme
in it and the card has the folder from the moment the core is
installed.

That is the second answer to this problem. The first was a trick: a
nonvolatile `nvram.bin` slot, which the host persists into that same
folder at Quit, power-off or sleep — creating the path on its way.
It worked, but only at exit, so a fresh card stayed read-only for its
whole first session. The firmware then tried to trigger it early,
dirtying the slot and flushing it whenever a create came back hollow.
On hardware that bought nothing: the host does not persist a
nonvolatile slot mid-session, and each attempt cost seconds of
commands nobody answered, so every open on a folderless card hung
before failing. The trick, the slot and the early flush are all gone.
Shipping a directory is what the problem always wanted.

**Some of this section is measured, not documented.** Every claim
about host behaviour is what one Pocket on one firmware did when we
poked it; any of it can change under a firmware update with nothing
anywhere saying so. When prose and `fstest.rp6502` disagree, the ROM
is right.

One claim already fell that way: "the host refuses to create under
Assets" stood here with a result code to its name, and then test
files turned up on a card under `/Assets/rp6502/common/`. The likely
truth is that the refusal was measured in the byte-order era — flags
arriving as `0x03000000` made every create a plain open of a missing
name, result 3 — and creates work wherever the folder already exists.
Assets' folder always exists, which is why the files are there. The
folder-missing behaviour was measured after that fix and stands.

**Seek is free.** Slot Read and Slot Write both carry a 32-bit offset
into the file, so random access needs no cursor protocol.

**Names must be absolute.** This one went round twice. First the
claim was that the host refuses a bare filename as malformed; then a
probe run looked like it resolved relative names against a pinned
`/Saves/rp6502/common/`, and the firmware was changed to lean on
that; then the next run showed a bare name simply never arriving
while the same name spelled in full worked. The original claim was
right. Whether an absolute path reaches anything outside `/Assets`
and `/Saves` is untested.

**Creating a file takes both flag bits.** Bit 0 on its own is answered
with a descriptor and makes nothing at all; bit 1, resize, is what puts
the file there. So a create carries both, which means it has to know
first whether the file already exists, since both bits against one that
does would cut it back to nothing. There is no flag for exclusive
creation either, and the same plain open answers both questions.

**The struct's integers are words, the path is bytes.** With
`bridge_endian_little` clear, byte zero of the path rides bits 31:24 of
its word — but the flags at 0x100 and the size at 0x104 are taken as the
bridge word itself, not assembled from that byte stream. Written low
byte first, flags of 3 arrive as `0x03000000`: every documented bit
clear, every reserved bit set. The host then opens the file and neither
creates nor resizes, which reads as success on a file already there and
as "not found" on one that is not.

**A growth can never prove a resize**, because a Slot Write past the end
produces the same length either way. Only a shrink can.

**Open File has two ways of saying yes** — 0 opened, 1 created and
opened — and only this command does. The others use 0 alone. The rest
are 2 slot not defined, 3 not found, 4 malformed path, 5 general.

**A write is not a save, but asking is safe now.** Slot Write returns
once the host has taken the bytes, which on a handheld that sleeps is
not the same as the card having them. 0x0188 would commit them, and no
firmware this core has met answers it — but the bridge override now
puts a ~0.9 s deadline on a data slot command the host never picks up,
so a flush costs one deadline instead of the session. The first sync
decides: a host that answers gets a real flush every time after, one
that does not is remembered for the session and a write is durable
when the host says it took it.

**Two deadlines, and the shorter one must be the bridge's.** Both the
bridge and `pocket_file` time a data slot command out, and for a while
the bridge's was the longer of the two on the reasoning that the
downstream should give up first. That is exactly backwards, and it
cost a real bug: the bridge is the side *parked* on the silent host,
and only its own retirement frees it. With the downstream quitting
first the bridge stayed parked with nobody listening, and the next
command — which proves it was accepted by watching `done` fall, and
finds it already low — adopted the flush's late answer as its own and
then executed at the host with no one waiting. A write reported as
failed, performed anyway. The deadlines are now 0.9 s bridge inside
1.8 s downstream, and a command the host ignores comes back as result
7 rather than a timeout.

**There is no mkdir, and no warning that there isn't.** Nothing in the
runtime API creates a directory, and the host does not invent the
folders in a path. Asked to create a file in a folder that is not
there it answers with a descriptor — the code for success — and no
file appears. There is no folder-creating act available at run time at
all: the nonvolatile-slot flush comes closest and only fires at exit.
So every directory the core needs has to ship in the package.

## On Analogue's documentation

Analogue are cunts about this. The openFPGA file API is published as a
table of command numbers, parameter offsets and result codes, and that
is the whole of it. Not one word about what the host actually does when
you send one. They shipped a filesystem interface and documented its
column headings.

What is missing is not detail, it is everything that decides whether
your code works:

- The two integers in Open File's parameter struct are read as bridge
  words while the path lying next to them in the same struct is read as
  a byte stream. Get that wrong and every flag you send is zero.
- The create bit on its own creates nothing. Resize is what makes the
  file. Nowhere.
- Creating into a folder that does not exist returns a **descriptor** —
  the success code — and writes nothing. Nothing in the API creates a
  folder either. So the documented way to make a file silently does not.
- There is a flush command, 0x0188, in the reference. Analogue's own
  reference core does not implement it and the console does not answer
  it. Issue one through the stock bridge and it never retires; every
  data slot command after it queues forever. One call killed the drive
  for the session until the override grew its own deadline. Not a word.
- Nothing anywhere distinguishes "that failed" from "that did nothing".

That last one is what actually costs. Errors are silent, so every
question has to be put to the physical device, and every trip is a
bitstream and a reboot. The swapped word above took a day — one line to
fix, a day to corner — and most of that day went on measurements that
proved nothing, because a file coming back the right length looks
identical whether the resize happened or a write past the end did it.
Only a shrink can tell those apart. Working that out is not engineering,
it is archaeology, and it was avoidable with one honest paragraph from
the people who wrote the firmware.

`fstest.rp6502` exercises the whole drive in one boot and prints its own
verdict, in the simulator as well as on the card. The build leaves it in
`build/verilator/release/tests/` with the other bring-up ROMs; they are not part of
the distribution. When this prose and that ROM disagree, the ROM is
right.

One correction, because it was earned. All of the above is about the
file API. The **hardware** they document properly: every external part
is named, with capacity, width, voltage and speed grade, at
<https://www.analogue.co/developer/docs/external-hardware>. This file
spent a long time asserting that a part number was something we were
never going to get, and the SDRAM controller was written against padded
guesses on that basis while a 256 KB SRAM sat tied off in `core_top.sv`
because nobody had read the page. That was our failure, not theirs.

## Reading the console

The machine's console — the 6502's `$FFE1` writes and the soft CPU's own
`com_printf`, interleaved — comes out two ways, both live in every
bitstream.

**Through the Pocket.** With debug logging switched on in the Pocket,
target command 0x0152 carries four console bytes per entry, first byte in
the top eight bits, so the 32-bit event id reads left to right as ASCII:
`52503635` is `RP65`. A short word at the end of a burst is
left-justified and zero-filled. One entry is a round trip through the
host, so the log lags and drops when the console outruns it; what it is
for is the boot narration and the last line before a hang.

**Through the debug pin.** `dbg_tx` is 115200 8N1, 1.8 V, on the 6515D
breakout board. Same bytes, no host in the path — which is the point: it
still talks when the PLL is dead, and silence there means something
specific.

## What the card tree is made of

`src/dist/pocket/` carries everything the card needs except the
binaries, which the build supplies:

- `Cores/Rumbledethumps.RP6502/core.bin` comes from the Quartus build
  through `src/gen/rbf_r_gen.py` (byte-wise bit reversal). `core.json`
  names it; the conventional name is `bitstream.rbf_r` and the loader
  reads whatever the manifest says.
- `Assets/rp6502/common/fonts.bin` is the glyph image, generated by
  `src/gen/vid_font_gen.py --emit-bin` from `vga/term/font.c`
  and dropped in the FPGA build tree. It carries every face and all
  seventeen code pages; nothing about the fonts is in the bitstream, so
  a core without it comes up with a blank screen. It loads near the top
  of the SDRAM, above the file slots' windows — the staging map climbs
  in slot id order, with the ROM first at address zero owning
  everything below its `size_maximum` ceiling — and the firmware copies
  it to the video device at every boot.
- `Assets/rp6502/common/oemcp.bin` and
  `Assets/rp6502/common/kbdlay.bin` ride the same way, from
  `src/gen/oem_table_gen.py` and `src/gen/kbd_layout_gen.py`. They are
  the code page conversion tables and the keyboard layouts, both far
  too large to link into a 96 KB tightly coupled memory and both read a
  word at a time through a window that cannot fetch anything wider than
  a byte. A core without either still runs: filenames fold to U+FFFD
  without the one, and the keys that type a character type nothing
  without the other while the arrows and the hotkeys go on working.
- `Cores/Rumbledethumps.RP6502/icon.bin` and
  `Platforms/_images/rp6502.bin` are here already, made from the logo by
  `src/gen/pocket_image_gen.py`. Analogue documents one format
  for both, under "Image Format" in its packaging-a-core page: sixteen
  bits a pixel, monochrome, brightness in the upper eight, the raster
  stored rotated a quarter turn counter-clockwise, no header and no
  magic. Only the size differs — 36x36 and 521x165 — and a file is the
  right length or it is wrong. Run the script again when the artwork
  changes and commit what it makes; `--selftest` re-encodes the two
  reference images in the core-template submodule and requires byte
  equality, which is what proves the rotation goes the right way.

## First bring-up

`core_top` takes two parameters. `TCM_INIT_FILE` names the soft CPU's
firmware image, four byte-lane files from
`src/gen/rv_tcm_gen.py`; the build supplies it and a bitstream
without it comes up fetching zeros, which looks exactly like a dead
video path and is not one. `CORE_TEST_PATTERN` replaces the picture with
colour bars while leaving the machine built and still talking on both
console paths, so one build separates "is the video path alive" from
"is the machine alive".

The firmware boots silently — the narration that used to print here
came out once the platform stopped needing bring-up — so the boot
diagnostic is now the failures it still reports and the states the
debug log shows:

| signal | what it says |
| --- | --- |
| *no 0x0152 traffic at all* | the soft CPU is not executing, or the host is not answering the log — turn on `CORE_TEST_PATTERN` to tell those apart |
| `oem: no tables` | the code page slot did not stage; accented filenames will fold to U+FFFD |
| `kbd: no layouts` | the layout slot did not stage; keys that type a character type nothing |
| `rom: bad image` | staging read back wrong — SDRAM, not the loader |
| the program's own output | everything worked and the 6502 is out of reset, so a black screen now is the video path |

From `src/rtl`, `cmake --preset pocket` then
`cmake --build --preset "Card package"` assembles the card tree into
`build/pocket/package`. Zip the three top directories at the archive
root as `Rumbledethumps.RP6502_<version>_<date>.zip`. The `Bitstream` build
preset stops one step earlier, at `build/pocket/bitstream/core.bin`.

That tree needs Quartus and `gcc-riscv64-unknown-elf` and nothing else — no
Verilator, no test suite.

## What each change costs

There is one target to ask for and no decision to make, because the three
things this build does cost wildly different amounts and CMake knows which
of them your edit touched.

Placing and routing is about nine minutes and turns on the RTL, the
constraints and the fitter assignments. Putting firmware into the
bitstream is about twenty seconds and turns on `src/rtl/sw`. Copying the
card tree is instant.

The firmware is cheap because it is not logic. It is the initial contents
of four M10K arrays, so a new image places nothing, routes nothing and
moves no timing arc, and the fit already on disk is still the fit that
comes out. Quartus keeps a MIF of each array under `db/`, generated from
`rv_soc`'s `$readmemh` while mapping, and those are what
`quartus_cdb --update_mif` reads back rather than the lane files they came
from. `rv_mif_gen.py` rewrites the four, `--update_mif` takes them into the
database, and the assembler makes a programming file out of the placement
that was already there. The claim was settled by fitting one firmware from
scratch and assembling another this way: the bytes match.

So the timing analyzer and the Design Assistant run with the fitter and
not with the firmware, which is correct rather than a shortcut — rerunning
them would re-measure a fit that has not changed. Nothing checks whether
the fit still matches the tree, either. That check was a hand-written gate
for a while, and it is now the shape of the build: a rule cannot run
against a stale input, and a check the build cannot forget beats one it
could.

One trap worth writing down, because it cost a wasted refit to find. **The
project file cannot be a dependency of the fit.** The fitter writes the
`.qsf` back — restamping the Quartus version, dropping the template's
`AUTO FIT` now that ours overrides it, rewriting every path relative — so
it differs from what CMake generated the moment a fit ends, and a fit that
depended on it would be out of date again before anyone typed anything.
`synth.cmake` stands in for it, being what decides its content.

`bitstream` is incremental now, so the honest loop is to type it whenever
unsure — an unchanged tree costs nothing, and a changed one needs the fit
anyway.

The bring-up ROMs — `fstest`, `file`, `bigfile`, `psg`, `opl`, `probe` —
are built into `build/verilator/release/tests/` and are deliberately not in the
package. Copy the ones you want into `Assets/rp6502/common/` on a card
when testing. The root-spelling `roots` probe answered its question —
relative names resolve against a pinned `/Saves/rp6502/common/` —
and retired.
