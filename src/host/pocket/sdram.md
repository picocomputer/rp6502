# The soft CPU's memory in SDRAM

Built, and it runs on a Pocket: a game runs, and a sleep wakes.

The soft CPU's memory was 96 KB of block memory, 96 of the FPGA's 308 M10K
blocks, with 305 in use. It is now in the SDRAM behind a small cache. The
design uses 219 blocks, so 89 are free.

## What is there

- **A ROM block and a RAM block**, as on a microcontroller. The ROM block is
  256 KB of code and constants, loaded by the Pocket from `firmware.bin`,
  and nothing writes it afterwards. The RAM block is 64 KB of data, heap and
  stack. The firmware's working data is 25 KB of it, the stack 4 KB, and the
  heap the 36 KB between.
- **One card file.** `firmware.bin` is 145 KB and holds the fonts, code
  pages and keyboard layouts. A firmware change runs no Quartus tool.
- **The main cache** (src/core/riscv/cache.sv) is for the two blocks alone.
  8 KB, 4 ways, 128 sets, lines of 16 bytes. A hit takes one clock and a
  miss about a microsecond. It costs 9 blocks.
- **A look-ahead** for everything else the soft CPU reads from the SDRAM:
  the ROM image, the file windows, the Get File scratch. It holds one
  16-byte line, and any write by the Pocket throws it away.
- **The savestate** saves the RAM block and never the ROM block: a wake
  loads `firmware.bin` again. The blob is 293 KB.
- **The build check.** The firmware carries its compile time and the blob
  carries it too. A wake whose blob names another build is refused.

### The SDRAM's map

The ROM slot is 62 MB and stays 62 MB: a whole ROM has to load into it, so
its size is a limit of the system. The top 2 MB is ours, and the map's gate
holds the line. In the 2 MB:

| Use | Size |
|---|---|
| The firmware's ROM block | 256 KB |
| The firmware's RAM block | 64 KB |
| The savestate window | 512 KB |
| 8 file windows | 256 KB |
| Get File scratch | 4 KB |

About a megabyte is left over.

### What it costs

The share of CPU time spent waiting on the cache, in simulation with the
real firmware:

| Work | CPU time waiting |
|---|---|
| Boot | 10% to 16% |
| A program running | 1.5% |
| PCM streaming | 1.2% |
| Heavy file work | 12% |

Timing after the last fit: setup 0.57 ns to spare, hold 0.11 ns.

## To do

### Wakes that fail

About 1 wake in 20 ends on a blank console, and so does a wake after the
card's firmware changed. Being fixed now:

- A savestate load that the Pocket asks for while it holds the core in
  reset is answered at once and run when the reset ends.
- A wake that does not resume starts the program over, and prints one line
  saying why.
- The simulated Pocket tries the wake orders real ones are known or
  suspected to use.

### Bitstream and firmware must match

Nothing stops a bitstream running a `firmware.bin` built for another one.
The bitstream shows two numbers in registers: the savestate version and a
register layout version, which also covers the memory map. The firmware
reads both first thing and stops if either differs from its own. The screen
shows the blinking cursor and nothing else. To be built with the wake fix.

### The 6502's memory on the die

The 6502's 64 KB lives in the external SRAM chip. It moves into block
memory, and the SRAM's pins are tied off as Analogue's template ties off an
unused part. The design then needs only the FPGA and the SDRAM, which is
what other boards have.

It takes 64 of the 89 free blocks and leaves 25. Where it fits in the order
is not decided.

### The font tables twice

The Pocket builds core's font tables in its RAM as the VGA does, 9 KB, and
then copies them into the video device's font memories, which are what it
draws from. The RAM copy is a duplicate that every savestate carries, and
it adds 11 ms to a boot.

Decided: the Pocket's linker script places core's font tables on the font
memories, and core's font builder writes whole 32-bit words.

- The tables in core/term/font.c go in a section of their own. Every other
  machine links them as it does today. The Pocket's link.ld puts the
  section on the font memories, which sit in the tables' order.
- The font memories take whole words and nothing reads them back. So the
  builder writes with its own loop of word stores through a volatile
  pointer, never `memcpy` or `memset`: the C library may copy a byte at a
  time, and the compiler may turn a plain loop back into `memcpy`.
- The code page copies are runs of 128 bytes at aligned places and become
  word copies as they are. The DEC graphics builder writes single bytes: it
  builds a row of 32 bytes in a local and writes the row out as words.
- The constant tables it copies from are aligned to 4, because a misaligned
  read on the soft CPU is a trap.
- `vid_fonts` and `vid_font` go. After a wake the font memories are empty,
  and the firmware builds them again for the saved code page with
  `font_load_code_page`, where it copied them from RAM.
- The same builder runs on the Pico, the VGA and the emulators, and their
  tests cover the tables' contents.
- No bitstream change unless the font memories' addresses have to move to
  match the tables' order.

### Tuning

- **The main cache's shape.** 8 KB and 4 ways is fast enough, and it gets
  no bigger. Try 4 KB, 2 ways, and replacing the least recently used line
  where it replaces lines in turn today, and shrink it if nothing slows.
- **The RAM block's size**, which the savestate carries.
- **The soft CPU at 50.4 MHz again.** It runs at 25.2 MHz because the
  fitter could not close timing into the CPU's registers, and it was
  close. With blocks free the fitter has room. The cache's hit path is
  longer than the block memory's was, which works against it.

### To clean up

- **The sleep tests are slow.** Every one boots the firmware, loads a ROM,
  saves and restores on its own. Boot once, keep the simulated machine, and
  start each test from there.
- **A save during a cache miss** is only tested at the whole-core level,
  where it was found. It wants a test beside the engine's own.
- **Hold margins in pocket.sdc** that nobody remembers the reason for.
- **PLL phase shifts written in rounded picoseconds.** Half of 19841 ps was
  written 9920 ps, and the analyzer timed the SDRAM capture against an edge
  1 ps after the launch. A rule in wiring.sdc names the right edge.

## For the next fit to confirm

- **The SDRAM capture's hold check.** The rule on dram_dq to
  pocket_sdram's dq_cap is a setup multicycle of two alone. Its hold
  multicycle of one moved the hold check a period early, where nothing
  can fail it, and is gone. The hold relation on that path should now
  read about 0 ns, the edge 1 ps after the launch, and its slack should
  be positive by about the chip's 2.5 ns of hold less the skew between
  dram_clk's pad and dq_cap's clock.
- **The savestate's four CSRs** make sst_engine's register memory 64
  deep where it was 32, and pocket_sst has a few more flops and three
  more crossings between clk_74a and clk_sys, all through registers
  named _s1.
- **The SDRAM's reset.** The controller is held in its power-up state
  by the PLL's lock, synchronised to clk_sys: one more fan-out of
  core_top's reset synchroniser.
- **pocket_file's arming.** One flop on clk_sys, set by a write to
  the slot register and cleared by a command or a savestate load. It
  gates the command's start.
