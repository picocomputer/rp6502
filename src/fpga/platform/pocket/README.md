# Porting to the Analogue Pocket

Notes for working on this core. Nothing here is needed to use it — the
distribution tree is described in `dist/rp6502.txt`.

## Suspend

`core.json` says `"sleep_supported": true`, and the design of sleep on
this platform is the reason it can: openFPGA sleep is the savestate
handshake — at sleep the host asks 0x00A0 for a blob, and this core
answers that it cannot make one, driven zeros on every savestate input
in `core_top.sv`. The Pocket powers down without a state and wake is a
fresh core load: bitstream, slots, everything restaged by the normal
boot. Waking to a reset machine is what a computer with a power switch
does, and it dissolves the old worry about the SDRAM staging store —
nothing has to survive because everything is reloaded.

Two things ride on sleep beyond the nap itself, both from Analogue's
data.json page: nonvolatile slots are flushed to the card "when a core
is stopped with the root menu Quit option, Pocket is turned off, or
Pocket is slept" — so sleep is also a save point for `nvram.bin` and
the folder it conjures below.

## The host's filesystem

`MSC0:` is the card, and the drive writes. The host resolves a
relative name against `/Saves/rp6502/common/` — a working directory
it pins there and never moves — and a leading slash names the card's
root, so the firmware strips the drive prefix and passes the rest
through verbatim. `foo.txt` and `MSC0:foo.txt` are the same saved
game; `MSC0:/Assets/rp6502/common/foo.txt` reaches the package's own
folder, which is writable. A program's plain `open("game.save", ...)`
lands in the same place on every platform.

The working directory is synthetic and pinned: getcwd answers
`MSC0:/Saves/rp6502/common/` — measured, not asked, since the host
has no such call — so appending a name to it opens the same file the
bare name does. chdir errors whatever it names, even that directory.
There is no delete, rename or mkdir: the target command list ends at
Open File, and those calls answer ENOSYS. Existence is probed with a
plain `O_RDONLY` open, which fails on a missing name without creating
anything; trying `save00.dat` upward is the directory listing this
host will ever have.

The folder problem that parked this work is solved by the nonvolatile
slot: `data.json` declares `nvram.bin` (slot 11, platform-common, init
to 0xFF), `msc_init` publishes its size into the data slot size table
through the write port that used to be tied to ground, and at the
first Quit, power-off or sleep the host persists the file — creating
`/Saves/rp6502/common/` on its way. That is a session too late for a
virgin card, so when a create comes back hollow the firmware asks
early, once a boot: it dirties the slot with a one-byte write, flushes
it, and retries the create. A host that honors either mid-session
turns a read-only first session into a working one; one that does not
leaves the drive read-only until that first exit, exactly as before.
Which kind this host is, a virgin-card `fstest` run answers.

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

**Names are not rooted after all.** "The host refuses a bare filename
as malformed" stood here too, and fell to the five-spelling probe
run: a relative name resolves against `/Saves/rp6502/common/`, a
working directory the host pins, and a leading slash starts at the
card's root. Whether a leading slash reaches anything outside
`/Assets` and `/Saves` is untested.

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
puts a ~1.8 s deadline on a data slot command the host never picks up,
so a flush costs one deadline instead of the session. The firmware's
first ask decides — a sync or the folder conjure, whichever comes
first: a host that answers gets a real flush every time after, one
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
failed, performed anyway. The deadlines are now 1.8 s bridge inside
3.6 s downstream, and a command the host ignores comes back as result
7 rather than a timeout.

**There is no mkdir, and no warning that there isn't.** Nothing in the
runtime API creates a directory, and the host does not invent the
folders in a path. Asked to create a file in a folder that is not
there it answers with a descriptor — the code for success — and no
file appears. The one folder-creating act the platform has is the
nonvolatile flush, which is what the `nvram.bin` slot is for — at
shutdown for certain, and mid-session if this host turns out to
answer a flush the firmware asks for when a create comes back
hollow.

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
`build/fpga/tests/` with the other bring-up ROMs; they are not part of
the distribution. When this prose and that ROM disagree, the ROM is
right.

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

`dist/` carries everything the card needs except the
binaries, which the build supplies:

- `Cores/Rumbledethumps.RP6502/bitstream.rbf_r` comes from the Quartus
  build through `src/gen/rbf_r_gen.py` (byte-wise bit reversal).
- `Assets/rp6502/common/fonts.bin` is the glyph image, generated by
  `src/gen/vid_font_gen.py --emit-bin` from `vga/term/font.c`
  and dropped in the FPGA build tree. It carries every face and all
  seventeen code pages; nothing about the fonts is in the bitstream, so
  a core without it comes up with a blank screen. It loads into the
  last 64 KB of the SDRAM, above the ROM slot's own ceiling, and the
  firmware copies it to the video device at every boot.
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
| `rom: bad image` | staging read back wrong — SDRAM, not the loader |
| the program's own output | everything worked and the 6502 is out of reset, so a black screen now is the video path |

`cmake --build build/fpga --target bitstream` then `--target package`
assembles the card tree into `build/fpga/tests/package`. Zip the three
top directories at the archive root as
`Rumbledethumps.RP6502_<version>_<date>.zip`.

The bring-up ROMs — `fstest`, `file`, `bigfile`, `psg`, `opl`, `probe` —
are built into `build/fpga/tests/` and are deliberately not in the
package. Copy the ones you want into `Assets/rp6502/common/` on a card
when testing. The root-spelling `roots` probe answered its question —
relative names resolve against a pinned `/Saves/rp6502/common/` —
and retired.
