# The soft CPU's memory in SDRAM

Built, and it runs on a Pocket: a game runs. The firmware-made savestate
waits for a fit.

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
- **Everything else is past the cache**, through the staging store: the
  ROM image, the savestate window, the file windows, the Get File
  scratch. Reads come through a look-ahead that holds one 16-byte line,
  and any write, the Pocket's or the soft CPU's, throws it away. The soft
  CPU's writes go to the controller a halfword at a time, DQM masking a
  byte it leaves out.
- **The savestate** is core's, made by the firmware into the blob window
  in the staging store, at the end of the SDRAM. A wake boots the firmware
  and then loads the blob. The blob is about 230 KB.
- **Bitstream and firmware must match.** The bitstream shows its register
  layout version in a register, and the firmware carries its own. It reads
  it first thing and stops if they differ, touching nothing: the screen
  stays black.
- **The 6502's 64 KB is in block memory**, 64 blocks, which leaves 25 free.
  The SRAM chip's pins are tied off as Analogue's template ties off an
  unused part, so the design needs only the FPGA and the SDRAM.

### The SDRAM's map

The ROM slot is 62 MB and stays 62 MB: a whole ROM has to load into it, so
its size is a limit of the system. The top 2 MB is ours, and the map's gate
holds the line. The 2 MB is packed from the bottom, and the savestate
window is last and runs to the end, so whatever the Pocket OS puts around
a savestate grows into empty space:

| Address | Use | Size |
|---|---|---|
| 0x03E00000 | The firmware's ROM block | 256 KB |
| 0x03E40000 | The firmware's RAM block | 64 KB |
| 0x03E50000 | Free, for the two blocks to grow | 192 KB |
| 0x03E80000 | 8 file windows | 256 KB |
| 0x03EC0000 | Get File scratch | 4 KB |
| 0x03EC1000 | The savestate window | 1276 KB |

The 512 KB from 0x03E00000 is the soft CPU's alone and the cache's window.

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

### Savestates

Built and simulated (`psleep`): a save, a wake into a fresh boot with the
load asked in reset, and a load into the running machine each run on
tick for tick with a session never saved; a file and the ROM's asset read
on from where they were; a save with a file open for writing answers
err; a bad savestate prints `bad savestate` and stops.

mtime stands still while the machine is held, so a save or a load costs
the program's clock nothing. The time of day comes from the RTC and
`UPTIME`, which never stops, so it follows real time. In simulation a save holds the machine for
0.38 s and a wake for 0.28 s.

For the Pocket to show: whether it takes busy at a save or load request
and ok at a load in reset; the order of a wake's file, its load request
and Reset Exit; what it does with a sleep the core refuses.

### A new ROM from the menu

The old program runs on while the Pocket writes the new ROM over its
file, and crashes. The Pocket sends 0x0082 (request write) for slot 0
before it writes; the core answers ok and ignores it, and the firmware
learns of the new ROM only at 0x008F. On 0x0082 for slot 0 the bitstream
holds the 6502 in reset at once; the firmware stops the program and
stages the new ROM at 0x008F as now. Goes in with the next fit.

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
  and the VID row builds them again for the saved code page with
  `font_load_code_page`.
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

- **The Pocket tests are past their mark.** The psleep set takes about
  72 seconds, as long as its slowest case. A round trip's time is mostly
  the load's sum and the save.

## For the next fit to confirm

The hold check on dram_dq to pocket_sdram's dq_cap should read a relation
of about 0 ns and positive slack, now that its hold multicycle is gone.

The 6502's RAM should take 64 M10K blocks and leave about 25 free, and the
paths from the 6502 into it and back should meet the multicycle of four.
