# RP6502 FPGA core

The Picocomputer 6502 as an FPGA core. The Analogue Pocket (openFPGA) is the
first target; MiSTer is planned. Both are Cyclone V, so `rtl/` stays platform
independent and each host gets a thin wrapper under `platform/`.

The 6502, VIA, video renderers and audio are RTL. A Hazard3 soft RISC-V runs a
trimmed build of the real `src/ria` firmware C for the OS layer — syscalls, HID
and ROM loading — mirroring the RP2350 + W65C02 split of the real machine.

## Audio

The PSG and the bell are RTL, proven bit-exact against `ria/aud/psg.c` and
`ria/aud/bel.c` in lockstep.

The OPL2 is `vendor/opl2_fpga`, Greg Taylor's reverse-engineered YM3812 under
LGPL-3.0, credited in the Pocket distribution README. emu8950 on the soft CPU
was never possible — it needs several times more cycles than the whole core
has — so the chip is RTL, and there is no bit-exact claim to make: it is a
different implementation from the emulator's, not a port of it. Its own tests
check the wiring rather than the waveform, which is what `test_opl` does.

It runs on `clk_sys` with the sample divider set to 1014, putting the rate
0.024% under the 49,715.9 Hz the chip specifies. That is deliberate: at its
native 12.727 MHz it would need a clock of its own, and this machine has paid
enough for a second clock domain already.

`aud_opl` presents what `aud_psg` presents, and `rp6502.sv` listens to
whichever pointer was programmed last, the way `aud_setup` hands the interrupt
over on real hardware. Four small fixes Quartus needs and Vivado did not live
in `vendor/opl2_fpga_rp6502`, each annotated where it sits.

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

## The soft CPU's own clock

Hazard3's frontend is the only block that cannot make 50.4 MHz — it wants
about 23 ns and has 19.8. Everything else on the fabric closes. So it runs at
25.2, half the machine, on `clk_rv`.

That clock comes from the PLL and not from a divider here, and the difference
is the whole story of why this took a second attempt. A toggle flop clocked on
`posedge clk_sys` produces a half-rate clock whose edge lands *after* this
module's own registers have settled at the same machine edge. A master clocked
that late samples a `bus_rdy` the machine has not published yet: it sets
`dph_waited` and retires the access in the very machine cycle the strobe first
went high, so `bus_stb` is never 1 at any machine edge and `bus_rsel`,
`stage_addr_q` and `regs_b_q` never latch. The read returns whatever the
previous window was driving.

Only accesses that *stall* are affected, because only they have a `bus_rdy`
that starts low and rises later — which is every even byte of the staged ROM,
the odd one being served from the held halfword with the ready already up. So
half the image came back as the previous window's data, `#!RP6502` failed to
compare, and the loader said `rom: bad image`. The bare machine escaped it
because its staging window never stalls. That is why this looked for a long
time like a `pocket_sdram` fault. It was not; the controller was always right.

The ratio is exactly 2:1 and edge aligned, so nothing here is asynchronous,
but a pulse still changes width in each direction and three places care:

- `bus_stb` is one soft-CPU clock, which is two of the machine's. The slave
  acts on the level, and acting twice pops a console ring twice and loses the
  byte between. Narrowed to its rising edge.
- `rv_soc_tx_valid` reaches a top-level port that testbenches count characters
  on. Same treatment, or every character counts twice.
- `slot_set` and `key_set` are one machine clock wide going the other way, and
  a pulse that narrow can fall between two soft-CPU edges and never be seen.
  Held for two it always spans one, and both land a value the far side is
  holding anyway, so being seen twice is harmless.

A testbench that drives `clk_rv` must rise it *with* `clk_sys`, never off a
flop, or it models the bug rather than the machine. It must also eval once
with both clocks low before the first edge: Verilator seeds its
previous-clock values inside the first `eval`, so an edge raised before that
is swallowed, and `rv_soc`'s asynchronous reset can miss its only window.
`tb_core.cpp` does this; two hand-rolled harnesses did not, and their first
subtest ran with the soft CPU's registers still at zero.

