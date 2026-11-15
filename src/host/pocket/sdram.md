# The soft CPU's memory in SDRAM

A plan. Nothing here is built.

The soft CPU's 96 KB is block memory: 96 of the FPGA's 308 M10K blocks, and
the design uses 305. Its memory moves to the SDRAM, grows to 256 KB, and a
small cache in block memory keeps it fast. About 87 blocks come back.

There is no way to load a build onto a Pocket from here, so everything is
proven in simulation, and a hardware run is a favor asked of the owner with
a yes, a no, a number or a photograph for an answer.

## What is there now

- **The CPU** is a Hazard3 on a 25.2 MHz clock with one bus. Its memory
  answers in one clock.
- **The firmware** is the first contents of that block memory, in the
  bitstream. Nothing loads it.
- **The SDRAM** is on a 50.4 MHz clock from the same PLL. Its controller
  reads and writes one 16-bit word at a time, seven clocks or more. The
  host's bridge is the only writer; the CPU and the savestate engine only
  read, a byte at a time, through the stage window at 0x60000000.
- **The host** writes every required data slot into the SDRAM, then lets the
  core out of reset. Fonts, code pages and keyboard layouts load this way.
- **A sleep** is a savestate and a wake is a new bitstream: the core starts
  fresh, the host loads the slots again, writes the saved blob into the
  SDRAM, and the engine puts it back. The blob holds the CPU's whole memory,
  which the engine reads and writes through a port of its own.

## The design

### The cache, in soc.sv

- **8 KB, 4 ways, 128 sets, lines of 16 bytes.** Ways and sets are
  parameters. A line is 16 bytes because the SDRAM row never splits one and
  eight words back to back is one visit to the chip.
- **A hit takes one clock**, as the block memory does now. All four ways are
  read at once and the tags pick one.
- **Write-back.** A store marks its line dirty. A dirty line goes to the
  SDRAM when another line needs its place.
- **A miss** holds the CPU, writes the old line out if it is dirty, reads
  the new one, and lets the CPU go.
- **Block memory:** 8 blocks of data and 1 of tags, valid and dirty bits,
  and which way goes next.
- **256 KB** of address space at 0, where the memory is now.

### The line port

The cache asks for whole lines: read this line, write this line, 128 bits
at a time. soc.sv gets that port in place of its memory arrays, and the
platform answers it.

- On the Pocket the SDRAM controller answers.
- In the benches that have no SDRAM, a few lines of C++ answer, the way they
  answer the stage window today.

### The SDRAM controller

- **A third client, for lines.** After the row is open it gives eight READs
  or eight WRITEs on eight clocks. The chip stays in the mode it is in, one
  word a command, so nothing else about it changes.
- **No byte writes.** The cache only ever writes whole lines.
- **Order:** refresh, the engine and the stage window, lines, the bridge.

### Where it lives in the SDRAM

256 KB comes off the top of the ROM slot, which is 63 MB: 0x03EC0000 to
0x03F00000. Nothing else moves.

### Boot

- The firmware is a required data slot, `firmware.bin`, at 0x03EC0000, in
  `Assets/rp6502/common/` beside the fonts.
- The core already stays in reset until the host has written the slots. It
  also waits for the bridge's last writes to reach the SDRAM.
- The cache empties itself as reset ends.
- The firmware leaves the bitstream. A firmware change needs no Quartus at
  all, and the build's MIF step goes.

### Savestate

- **Save.** With the CPU halted, the cache writes out every dirty line. The
  engine then reads the CPU's memory from the SDRAM through the line port, a
  line for four words.
- **Load.** The engine writes the CPU's memory into the SDRAM through the
  line port, then the cache empties itself before the CPU resumes.
- **The blob** grows from 325 KB to 485 KB and still fits its 640 KB window.
  Its version number changes, so an old sleep does not wake.

### The firmware

- The linker script says 256 KB. The stack goes to the top.
- Nothing else changes: the stage window, the file windows and the assets
  stay where they are.

## Later, not now

Fonts, code pages and keyboard layouts as ordinary memory instead of bytes
read through the stage window. The cache can cover them, since the host
writes them before the CPU starts. The file windows cannot go the same way:
the host writes those while the CPU runs, behind the cache's back.

## Steps

Every step ends with the simulation passing.

1. **The SDRAM model** takes back-to-back READs and WRITEs, and checks them.
2. **The controller's line client**, tested alone against the model: lines
   against the old clients, refresh and self refresh.
3. **The cache**, tested alone against a C++ memory: hits, misses, dirty
   lines, every way, the flush and the empty.
4. **soc.sv with the cache**, and the benches' C++ line port. Every test
   that boots firmware passes again.
5. **The Pocket core**: line port to the controller, the firmware slot, the
   reset wait, the map. The whole core boots the real firmware from the
   SDRAM model and runs the 6502 tests.
6. **The savestate engine**: flush, read and write through the line port,
   empty. The sleep tests pass.
7. **Measure** in simulation: misses and time lost in terminal output, API
   calls, PCM and file transfer. Set ways and sets from that.
8. **Fit.** Timing, and the block memory count.
9. **Hardware**, by the owner: does it boot, does a program run, does a
   sleep wake.

## The debug pin

The README says the firmware's log goes out a pin to "the USB UART on the
6515D breakout board". That name is Analogue's comment on pins K21 and K22
in their template; nobody here has such a board. The README gets corrected.
