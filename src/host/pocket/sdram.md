# The soft CPU's memory in SDRAM

Built, and it runs on a Pocket: games run, sleep and wake resume, and
Memories save and load.

## What is there

- **A ROM block and a RAM block**, as on a microcontroller. The ROM block is
  256 KB of code and constants, loaded by the Pocket from `firmware.bin`,
  and nothing writes it afterwards. The RAM block is 64 KB of data, heap and
  stack.
- **One card file.** `firmware.bin` is about 160 KB and holds the fonts,
  code pages and keyboard layouts. A firmware change runs no Quartus tool.
- **The main cache** (src/core/riscv/cache.sv) is for the two blocks alone.
  8 KB, 4 ways, 128 sets, lines of 16 bytes. A hit takes one clock and a
  miss about a microsecond. It costs 9 blocks.
- **Everything else is past the cache**, through the staging store: the
  ROM image, the file windows, the Get File scratch and the savestate
  window. Reads come through a look-ahead that holds one 16-byte line, and
  any write throws it away.
- **The savestate is core's**, as on every other machine. One Pocket row,
  `FAB_` in sw/fab.c, carries what only the bitstream holds. The firmware
  saves at a clean point in its main loop with the machine held, and a
  wake boots normally and then loads. It is about 235 KB.
- **The font tables are built where the video draws them from.** The
  Pocket's link.ld places core's tables on the font memories.
- **The 6502's 64 KB is in block memory.** The SRAM chip is tied off, so
  the design needs only the FPGA and the SDRAM.
- **A new ROM from the menu stops the old one** as soon as the Pocket asks
  to write the ROM slot.
- **Bitstream and firmware must match.** The firmware reads the
  bitstream's register layout version first thing and stops, touching
  nothing, if it differs from its own.

### The SDRAM's map

The ROM slot is 62 MB and stays 62 MB: a whole ROM has to load into it, so
its size is a limit of the system. The top 2 MB is ours, and the map's gate
holds the line. The savestate window is last and runs to the end, so the
savestate can grow into empty space:

| Address | Use | Size |
|---|---|---|
| 0x03E00000 | The firmware's ROM block | 256 KB |
| 0x03E40000 | The firmware's RAM block | 64 KB |
| 0x03E50000 | Free, for the two blocks to grow | 192 KB |
| 0x03E80000 | 8 file windows | 256 KB |
| 0x03EC0000 | Get File scratch | 4 KB |
| 0x03EC1000 | The savestate window | 1276 KB |

The 512 KB from 0x03E00000 is the soft CPU's alone and the cache's window.

### What the Pocket showed

- A load writes the savestate back at the window's start, with no header.
  The Memories file on the card has one; the core never sees it.
- While its load request is unanswered, the Pocket serves no file command.
  The firmware answers once the savestate checks out, then restores.

### Size and timing

The soft CPU runs at 50.4 MHz on clk_sys, the machine's clock. The last
fit: 14,051 ALMs (76%), 283 of 308 M10K blocks, setup 0.43 ns to spare,
hold 0.08 ns. Removing the snapshot engine, the gated clock and the
CPU's own clock gave back 1,992 ALMs.

The share of the soft CPU's clocks spent waiting on the cache, in
simulation at 50.4 MHz, for the shape built and three smaller ones. A miss
costs about 35 clocks.

| Work | 8 KB 4-way | 8 KB 2-way | 4 KB 4-way | 4 KB 2-way |
|---|---|---|---|---|
| Reset Exit to the 6502 starting | 23-34% | 24-35% | 35-39% | 35-39% |
| A program calling the API in a loop | 16% | 21% | 41% | 58% |
| The main loop idle, audio playing | 0.05% | 21% | 38% | 46% |
| Heavy file work | 12% | 20% | 40% | 45% |
| A wake, Reset Exit to the machine let go | 4.8% | 5.3% | 9.4% | 10% |

8 KB 4-way stays: every smaller shape waits several points more, and 2
ways cannot hold the idle main loop. A save holds the machine 71 ms and a
wake 41 ms whatever the shape.

## To do

### To clean up

- **The Pocket tests.** The set takes about 80 seconds, as long as its
  slowest case. A round trip's time is mostly the save and the load.
