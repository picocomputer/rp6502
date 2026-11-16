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

The last fit: 14,399 ALMs (78%), 283 of 308 M10K blocks, setup 0.53 ns to
spare, hold 0.09 ns. Removing the snapshot engine and the gated clock gave
back 1,644 ALMs.

The share of CPU time spent waiting on the cache, in simulation:

| Work | CPU time waiting |
|---|---|
| Boot | 10% to 16% |
| A program running | 1.5% |
| PCM streaming | 1.2% |
| Heavy file work | 12% |

## To do

### Tuning

- **The main cache's shape.** 8 KB and 4 ways is fast enough, and it gets
  no bigger. Try 4 KB, 2 ways, and replacing the least recently used line
  where it replaces lines in turn today, and shrink it if nothing slows.
- **The soft CPU at 50.4 MHz.** It runs at 25.2 MHz. A first fit with only
  the clock raised missed by 0.3 to 0.4 ns, all inside Hazard3's decode
  (`cir` to `x_rs1_select_regs_xm_mw`). Done properly, the CPU moves onto
  the machine clock and the half-speed handshakes and the falling-edge
  capture in wiring.sv go. Hazard3's reduced-bypass option does not touch
  the failing path.

### To clean up

- **The Pocket tests.** The set takes about 80 seconds, as long as its
  slowest case. A round trip's time is mostly the save and the load.
- **The SDRAM capture's hold check** (dram_dq to dq_cap) has not been read
  out of a fit report since its hold multicycle went.
