# The soft CPU's memory in SDRAM

Built, and it runs on a Pocket: a game runs, and a sleep wakes.

The soft CPU's memory was 96 KB of block memory, 96 of the FPGA's 308 M10K
blocks, with 305 in use. It is now 256 KB of the SDRAM behind a small cache.
The design uses 219 blocks, so 86 came back.

## What was built

- **The cache** (src/core/riscv/cache.sv). 8 KB, 4 ways, 128 sets, lines of
  16 bytes, one cache for code and data. A hit takes one clock, as the block
  memory did. A store marks its line dirty, and a dirty line goes to the
  SDRAM when another needs its place. It costs 9 blocks.
- **Lines in the SDRAM controller.** A third client reads or writes a
  16-byte line as eight words back to back. The cache only writes whole
  lines, so the SDRAM needs no byte writes.
- **Where it lives.** The top 256 KB of the ROM slot's 63 MB, at 0x03EC0000.
- **Boot.** The firmware is a file, `firmware.bin`, that the Pocket loads
  into the SDRAM before the core leaves reset, as it loads the fonts. The
  core waits until the last of it has landed. The firmware is out of the
  bitstream, so a firmware change runs no Quartus tool.
- **A running machine's memory is its own.** Once the core runs, a host
  write into the CPU's memory is dropped, because the cache would not know.
- **Savestate.** The engine has the cache write its dirty lines out, then
  saves the memory from the SDRAM a line at a time. A load writes it back
  and empties the cache. The blob went from 325 KB to 485 KB, inside its
  640 KB window, and its version went to 8.
- **The firmware** links for 256 KB and has 169 KB of heap where it had 5.

## What it costs

Measured in simulation with the real firmware. A miss takes about a
microsecond.

| Work | Misses | CPU time waiting |
|---|---|---|
| Boot | 0.6% to 1.1% | 10% to 16% |
| A program running | 0.1% | 1.5% |
| PCM streaming | 0.1% | 1.2% |
| Heavy file work | 0.9% | 12% |

## What it turned up

- **SDRAM reads were captured late.** The controller took read data more
  than a clock after the chip had let go of the bus, which worked because
  the bus keeps its charge. Reads are now taken on the chip's own clock
  edge, where the datasheet puts the data. The chip model runs on the chip's
  clock and holds data only in its real window, so a late capture fails in
  simulation.
- **A save could lose a CPU register.** A halted CPU still sends a fetch,
  which can now wait on a miss, and the engine read the CPU's not-ready as
  an instruction taken. The debug port waits while a fetch is on the bus.
- **The fitter split pcm's accumulator.** It packed the register into a
  multiplier's DSP block and kept a copy in logic, and the copy arrived
  40 ps too soon. The accumulator is kept out of the DSP block.
- **The debug pin.** The README named a "6515D breakout board" for the
  log's UART. That is Analogue's comment on pins K21 and K22 in their
  template; no such board is here. The README says so.

Timing after the last fit: setup 0.78 ns to spare, hold 0.115 ns.

## ROM, RAM and one file

Built and passing in simulation. Not yet run on a Pocket.

What it measured, in simulation with the real firmware:

- `firmware.bin` is 145 KB and the only card file.
- The blob is 293 KB. It was 485 KB with the whole memory in it, and 325 KB
  before this work.
- The firmware's working data is 25 KB of the 64 KB RAM block, the stack
  4 KB, and the heap the 36 KB between.
- The look-ahead makes one trip to the SDRAM for four word reads where two
  byte reads took one. A ROM load in the file test is 86,000 CPU clocks
  shorter.

### The SDRAM's map

The ROM slot is 62 MB and stays 62 MB: a whole ROM has to load into it, so
its size is a limit of the system. The top 2 MB is ours, and the map's gate
holds the line. In the 2 MB:

| Use | Size |
|---|---|
| The firmware's ROM block | 256 KB |
| The firmware's RAM block | 64 KB or more |
| The savestate window | the blob and the Pocket's 53 KB around it |
| 8 file windows | 256 KB |
| Get File scratch | 4 KB |

About a megabyte is left over.

### A ROM block and a RAM block

The soft CPU's memory becomes two blocks, as on a microcontroller.

- **The ROM block** is code and constants. The Pocket loads it from
  `firmware.bin`, and nothing writes it afterwards.
- **The RAM block** is data, heap and stack. It is no data slot, since
  nothing loads it. The startup code copies the data segment's first values
  from the ROM block and clears the rest.

The linker script has the two regions. The savestate saves the RAM block,
whole, and never the ROM block: a wake loads `firmware.bin` again.

### One file

Fonts, code pages and keyboard layouts are constant arrays in core that
every other machine links, tagged `HOST_IN_FLASH`. The Pocket links them
too, and they land in the ROM block. Its own font, code page and layout
readers go, with their three data slots, three card files, three build
steps and three "table missing" failures. `firmware.bin` grows from 71 KB
to about 145 KB and is the only card file, so it needs no underscore.

### The build check

The firmware carries its compile time at a fixed place, and the blob carries
it too. A wake whose blob names another build is refused, as an old blob
version is. It catches a card whose firmware changed while the Pocket slept.

### Two caches

- **The main cache** is for the ROM and RAM blocks alone. Data never passes
  through it, so a burst of file data cannot push the firmware's code out.
- **A look-ahead** for everything else: the ROM image, the file windows,
  the Get File scratch. It holds one 16-byte line, read-only. One trip to
  the SDRAM fetches the line and the reads that follow are answered from
  it, a word at a time, where every byte is its own access today. Any write
  by the Pocket throws the line away, in hardware, so the firmware has no
  rule to remember. About 150 flops and no block memory.

Fetching the next line ahead of the CPU is left until the one-line version
is measured.

### Order

1. The map: the ROM slot to 62 MB, the blocks and windows placed in the
   2 MB, the gate.
2. ROM and RAM blocks: the linker script, the startup copy, the cache's
   address map, the savestate saving the RAM block alone, the build check.
3. One file: the tables in the ROM block, the Pocket's readers and slots
   gone.
4. The look-ahead, and the firmware reading words through it.
5. Fit, then the Pocket: boot, a program, a sleep and a wake, a wake after
   the card's firmware changed.
6. The font tables twice. With the fonts in the firmware, the Pocket builds
   core's font tables in its RAM as the VGA does, 9 KB, and then copies them
   into the video device's font memories, which are what it draws from. The
   RAM copy is a duplicate that every savestate carries, and it adds 11 ms
   to a boot. The font builder should write to where a machine keeps its
   fonts. The shape is to be talked through first.

## Tuning, after that runs on a Pocket

- **The main cache's shape.** 4, 8 and 16 KB; 2 and 4 ways; and replacing
  the least recently used line where it replaces lines in turn today.
- **The RAM block's size**, which the savestate carries.
- **The soft CPU at 50.4 MHz again.** It runs at 25.2 MHz because the
  fitter could not close timing into the CPU's registers, and it was
  close. With 86 blocks free the fitter has room. The cache's hit path is
  longer than the block memory's was, which works against it.

## To clean up

- **The sleep tests are slow.** Every one boots the firmware, loads a ROM,
  saves and restores on its own; the 18 of them are 900 of the suite's 1,160
  CPU seconds. Boot once, keep the simulated machine, and start each test
  from there.
- **A save during a cache miss** is only tested at the whole-core level,
  where it was found. It wants a test beside the engine's own.
- **Hold margins in pocket.sdc** that nobody remembers the reason for.
- **PLL phase shifts written in rounded picoseconds.** Half of 19841 ps was
  written 9920 ps, and the analyzer timed the SDRAM capture against an edge
  1 ps after the launch. A rule in wiring.sdc names the right edge.
