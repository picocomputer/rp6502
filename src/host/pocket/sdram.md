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

## The SDRAM's map

The ROM slot is 62 MB and stays 62 MB: a whole ROM has to load into it, so
its size is a limit of the system. The top 2 MB is ours, for the CPU's
memory, the savestate window, the file windows and the card files. Nothing
may take from the ROM slot again, and the map's gate checks it.

Today the ROM slot is 62.75 MB and the CPU's memory sits in what was its
top 256 KB, because the last megabyte had only 48 KB free. Step 2 below
moves it.

## Next

1. **An underscore in front of the four card files**, so they sort apart
   from the ROMs in `Assets/rp6502/common`: `_firmware.bin`, `_fonts.bin`,
   `_oemcp.bin`, `_keyboard.bin`.
2. **The map.** The ROM slot ends at 62 MB, the CPU's memory moves into the
   top 2 MB, and the gate holds the ROM slot at 62 MB.
3. **The cache's shape.** Measure 8 KB 2-way, 4 KB 4-way and 4 KB 2-way
   against the 8 KB 4-way it has.
4. **A smaller savestate.** Either 128 KB of memory instead of 256 KB, or
   keep 256 KB and leave the code out of the blob: code never changes and
   a wake loads `firmware.bin` again, so only data, heap and stack need
   saving. The second needs the blob to carry the firmware's build, and a
   wake to refuse a blob from another build.
5. **The soft CPU at 50.4 MHz again.** It runs at 25.2 MHz because the
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
