# Porting to the Analogue Pocket

Notes for working on this core. Nothing here is needed to use it — the
distribution tree is described in `dist/rp6502.txt`.

## Suspend

`core.json` says `"sleep_supported": false`, and it stays false until
someone does the work below rather than because the flag is hard to
change.

The machine has no state worth saving and no way to save it: everything
it is lives in the fabric, and there is no nonvolatile slot to put it
in. That part is fine — waking to a reset machine is what a computer
with a power switch does.

What is not fine is the staged ROM. `src/fpga/sw/rom.c` reads assets
straight out of the SDRAM staging window for as long as the program
runs, and the glyphs sit in the last 64 KB of the same store. A load
does not finish; it keeps going. So sleep is only safe if either the
SDRAM is still being refreshed while the Pocket is asleep, or the core
restages the slot on waking before it lets the machine run. Neither is
established here, and the first is not ours to decide.

Whoever picks this up: prove which of the two it is first, because a
core that wakes reading a store nobody refreshed will not fail loudly
— it will read plausible rubbish and hand it to a running program.

## The host's filesystem

**Everything in this section is a guess.** None of it is documented.
Every line is what one Pocket on one firmware did when we poked it, and
some of it is probably still wrong — three of the claims that stood here
earlier were, and each was written just as confidently as what replaced
it. Any of it can change under a firmware update with nothing anywhere
saying so.

`MSC0:` is `/Assets/rp6502/common/` — the same folder the fonts and the
code page tables load from. A drive rooted in `Saves` would read better
and is what the folder is for, but nothing we could find creates it, so
the machine writes where the card already has somewhere to write.

**Seek is free.** Slot Read and Slot Write both carry a 32-bit offset
into the file, so random access needs no cursor protocol.

**Names must be rooted.** The host takes `/Assets/<platform>/common/name`
or `/Saves/<platform>/common/name` and refuses anything else as
malformed — including a bare filename that names a file which exists,
which is how we know it rejects the form and not the lookup. Only
platforms listed in `core.json` are reachable.

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

**A write is not a save, and there is nothing to be done about it.**
Slot Write returns once the host has taken the bytes, which on a
handheld that sleeps is not the same as the card having them. 0x0188
would commit them and this host does not answer it: a core that issues
one waits out its timeout, and then every later command times out too,
because the bridge puts no deadline on a data slot operation and never
leaves one. One flush takes the drive down for the rest of the session.

**There is no mkdir, and no warning that there isn't.** Nothing creates
a directory, and the host does not invent the folders in a path. Asked
to create a file in a folder that is not there it answers with a
descriptor — the code for success — and no file appears. Which is why
`MSC0:` is rooted in `Assets`.

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
  it. Issue one and it times out; every data slot command after it times
  out too, because the bridge has no deadline on one and never leaves
  it. One call kills the drive for the session. Not a word.
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
  build through `src/fpga/codegen/rbf_r_gen.py` (byte-wise bit reversal).
- `Assets/rp6502/common/fonts.bin` is the glyph image, generated by
  `src/fpga/codegen/vid_font_gen.py --emit-bin` from `vga/term/font.c`
  and dropped in the FPGA build tree. It carries every face and all
  seventeen code pages; nothing about the fonts is in the bitstream, so
  a core without it comes up with a blank screen. It loads into the
  last 64 KB of the SDRAM, above the ROM slot's own ceiling, and the
  firmware copies it to the video device at every boot.
- `Cores/Rumbledethumps.RP6502/icon.bin` and
  `Platforms/_images/rp6502.bin` are here already, made from the logo by
  `src/fpga/codegen/pocket_image_gen.py`. Analogue documents one format
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
`src/fpga/codegen/rv_tcm_gen.py`; the build supplies it and a bitstream
without it comes up fetching zeros, which looks exactly like a dead
video path and is not one. `CORE_TEST_PATTERN` replaces the picture with
colour bars while leaving the machine built and still talking on both
console paths, so one build separates "is the video path alive" from
"is the machine alive".

The firmware narrates its own boot, and where the log stops is the
answer:

| last line | what got that far |
| --- | --- |
| *nothing* | the soft CPU is not executing, or the host is not answering 0x0152 — turn on `CORE_TEST_PATTERN` to tell those apart |
| `boot: rv` | it runs; `term_init` did not return |
| `boot: term` | the drivers are up; `vid_init` did not return, and it is the one that copies the font out of SDRAM |
| `boot: loading` | it reached the slot; the loader did not finish |
| `rom: bad image` | staging read back wrong — SDRAM, not the loader |
| `boot: running` | everything above worked and the 6502 is out of reset, so a black screen now is the video path |

`cmake --build build/fpga --target bitstream` then `--target package`
assembles the card tree into `build/fpga/tests/package`. Zip the three
top directories at the archive root as
`Rumbledethumps.RP6502_<version>_<date>.zip`.

The bring-up ROMs — `fstest`, `file`, `bigfile`, `psg`, `opl`, `probe` —
are built into `build/fpga/tests/` and are deliberately not in the
package. Copy the ones you want into `Assets/rp6502/common/` on a card
when testing.
