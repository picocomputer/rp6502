# RP6502 FPGA core

The Picocomputer 6502 as an FPGA core. The Analogue Pocket (openFPGA) is the
first target; MiSTer is planned. Both are Cyclone V, so `rtl/` stays platform
independent and each host gets a thin wrapper under `platform/`.

The 6502, VIA, video renderers and audio are RTL. A Hazard3 soft RISC-V runs a
trimmed build of the real `src/ria` firmware C for the OS layer — syscalls, HID
and ROM loading — mirroring the RP2350 + W65C02 split of the real machine.

## Audio

The PSG and the bell are RTL, proven bit-exact against `ria/aud/psg.c` and
`ria/aud/bel.c` in lockstep. OPL2 is not ported: no permissively licensed RTL
implementation exists (JTOPL2 is GPL-3, opl3_fpga is LGPL-3), and emu8950 on
the soft CPU needs several times more cycles than the whole core has. Programs
that select the OPL get an xreg failure, the signal for an absent device.

## Layout

    rtl/core/       the machine, independent of the FPGA platform
    platform/       per-host wrappers (Pocket APF, MiSTer), Quartus projects

Tests live with every other test, in `tests/fpga` — the Verilator testbench and
the emu_core reference oracle.

## Building the simulation

Development is simulation first. Tests run the same `.rp6502` on the verilated
machine and on `emu_core`, then compare — the emulator is the reference for
behavior the RTL must reproduce.

    sudo apt-get install verilator gtkwave
    cmake -B build/fpga -S src/fpga
    cmake --build build/fpga
    ctest --test-dir build/fpga

Without Verilator only the oracle tests build; CMake warns and continues.

Set `RP6502_FPGA_TRACE` to a path to capture an FST trace, viewable in gtkwave:

    RP6502_FPGA_TRACE=rtl.fst ./build/fpga/tests/test_rtl

## Measuring the render budget

`test_budget` counts, for every scanline of the heaviest fixtures, the clocks
from the line boundary to the last engine going idle, and reports XRAM port A's
occupancy beside it. The beam's deadline is pixel 799 — the next line's pixel
zero is read then — so a line is `4 * 799` clocks today and would be `2 * 799`
at half the system clock.

Read those numbers as a floor. Every fixture stops its 6502 before the
measurement and none of them make sound, so neither takes the port A slot a
running machine would, and no fixture puts a heavy sprite load over mode 3's
XRAM-palette prologue.

## Deferred: gate the palette cache on width and depth

Mode 5 fetches a paletted sprite's colour from XRAM once per pixel — request,
grant, data, emit — which is what its time goes on, and why reading the index
word ahead bought it nothing. Mode 3 already does the other thing: it pulls the
whole palette into a small RAM beside the engine once, then indexes it
combinationally.

Neither choice is right unconditionally, and both terms are known when the
descriptor is decoded. A palette is `2^(bpp-1)` words — two 16-bit entries to a
word — against however many pixels the clipped span will emit:

| bpp | palette words | cache wins above a span of |
| --- | --- | --- |
| 1 | 1 | 1 |
| 2 | 2 | 2 |
| 4 | 8 | 8 |
| 8 | 128 | 128 |

So at 1, 2 and 4 bits a cache pays for anything but the narrowest sprite, and
at 8 bits it only pays for one wider than 128 — which is why mode 3, always
full width, can cache unconditionally and a sprite engine cannot. Sprites are
usually 8 or 16 wide.

The same comparison applies to mode 1 and mode 2, whose windows can be narrow
enough that mode 3's unconditional load is the wrong trade for them too.

Worth doing when the render needs the clocks. It does not today.

## Unfinished: the soft CPU's own clock

Hazard3's frontend is the only block that cannot make 50.4 MHz — it wants
about 23 ns and has 19.8. Everything else on the fabric closes. The intended
answer is to run it at 25.2, half the machine, which it has room for several
times over.

The domain crossing itself is exactly 2:1 and edge aligned, so nothing about
it is asynchronous, but a pulse changes width in each direction and three
places care:

- `bus_stb` is one soft-CPU clock, which is two of the machine's. The slave
  acts on the level, and acting twice pops a console ring twice and loses the
  byte between. Narrow it to its rising edge.
- `rv_soc_tx_valid` reaches a top-level port that testbenches count characters
  on. Same treatment, or every character counts twice.
- `slot_set` and `key_set` are one machine clock wide going the other way, and
  a pulse that narrow can fall between two soft-CPU edges and never be seen.
  Held for two it always spans one, and both land a value the far side is
  holding anyway, so being seen twice is harmless.

All three were written and work. What does not work is the staged ROM load
through the real SDRAM: at half rate the loader reports `rom: bad image`,
where at full rate with the same adaptations it reports `rom: staged`. The
bare machine, whose staging window is served by the testbench with no
latency, loads correctly at half rate — so it is the pocket_sdram handshake
that minds a master slower than the bus, not the crossing.

`pocket_sdram_rvalid` is already qualified by the address it holds, so the
obvious stale-hold race is not the answer, and the next person should start
by watching `stage_pend`, `stage_rvalid` and `stage_stall` across one byte
fetch with the divider in.

Reproduce by dividing clk_sys by two into `rv_soc`'s clock in rp6502.sv and
running test_pocket with POCKET_DEBUG=1.
