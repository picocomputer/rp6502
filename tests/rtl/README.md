# The machine in simulation, and as an FPGA core

This directory verilates the machine and runs the suite that only a simulator
can answer. It is also where the FPGA work is written down: the Picocomputer
6502 as an FPGA core, with the Analogue Pocket (openFPGA) the first target and
MiSTer planned. Both are Cyclone V, so `src/core` stays platform independent
and each host gets a thin wrapper in `src/host/`.

The 6502, VIA, video renderers and audio are RTL. A Hazard3 soft RISC-V runs a
trimmed build of the real `src/host/pico/ria` firmware C for the OS layer —
syscalls, HID and ROM loading — mirroring the RP2350 + W65C02 split of the
real machine.

## Audio

The PSG is RTL and agrees with `core/aud/psg.c` sample for sample in lockstep.
Its ninth voice is the console bell, configured by the soft CPU: the sounds
are `core/aud/bel_presets.c` and the queue and lifetime are
`src/host/pocket/sw/bel.c`, so fabric holds a voice and software holds the bell.

Nothing gates the mix. Every engine and the bell sum, on one sample tick —
the PSG's divider — and an engine with no program answers zero.

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

## What the RTL is made of

`src/core` holds the machine, C and SystemVerilog together — see its README for
how that tree is arranged. The wrapper binding it to one board is
`src/host/pocket`, and the soft CPU's firmware is `src/host/pocket/sw`. MiSTer
arrives as `src/host/mister`; nothing in the machine changes.

Tests are filed by claim. `tests/cpu` is the machine's, written once and run
against whichever machine a tree builds; `tests/rtl` is what only a simulator
can answer, and is also the root that builds the verilated machine;
`tests/host/pocket` carries what is genuinely about this board rather than
about the machine. `tests/bench` carries the Verilator testbench and the
two `mut.h` bindings.

## Building the simulation

Development is simulation first. A test runs its `.rp6502` against whichever
machine its tree built and checks what came back against the expectation
written down beside it — neither machine is the other's reference.

    sudo apt-get install verilator gtkwave ninja-build
    cd tests/rtl
    cmake --preset release
    cmake --build --preset release
    ctest --preset release

Ninja is required, not preferred, and CMake stops if it is missing: this is
two dozen test binaries over a handful of shared verilated models, and make
builds one recipe at a time unless told otherwise.

Each model is verilated once, into a library the tests link. It used to be
verilated once per test, which meant nineteen identical elaborations of the
machine and nineteen compiles of the five megabytes of C++ each one produced
— half the build, spent making the same thing twenty times. The tests that
take the longest are registered a case at a time, so the suite is bounded by
its slowest case rather than by its slowest binary.

## The two trees

Two roots include the machine's CMake modules from `src/core`:

| root | builds in | what it is for |
| --- | --- | --- |
| `tests/rtl` | `build/rtl` | the simulation and its whole suite |
| `src/mach/pocket` | `build/pocket` | the Analogue Pocket core, and `synth` for area and timing |

A Debug simulation is `-DCMAKE_BUILD_TYPE=Debug` on the first of those, for
the rare case of stepping a testbench.

**A bitstream needs Quartus and `gcc-riscv64-unknown-elf`, and nothing
else.** No Verilator, no host test suite. That was not true for a while:
the Quartus projects were built from a source list defined inside a
`verilator_FOUND` guard, so a machine without a simulator had no bitstream
target at all — including the CI runner whose whole job is fitting one. The
list moved to `machine.cmake`, which both roots include, and it is
still the list the simulation verilates, so the thing measured is still the
thing tested.

The simulation, for its part, requires Verilator rather than warning and
carrying on: a tree that exists to run the RTL has nothing to offer without
it, and the C suite it used to fall back to is `tests/cpu`, which runs in the
emulator's own build.

Set `RP6502_RTL_TRACE` to a path to capture an FST trace, viewable in gtkwave:

    RP6502_RTL_TRACE=rtl.fst ./build/rtl/vid/test_raster

`test_raster` is the one built with `TRACE_FST`, because the trace costs
simulation speed and only the test that reads waveforms wants it.

## Measuring the render budget

`test_modes` counts, for every scanline of the heaviest fixtures, the clocks
from the line boundary to the last engine going idle, and reports XRAM port A's
occupancy beside it. The beam's deadline is pixel 799 — the next line's pixel
zero is read then — so a line is `2 * 799` clocks today and would be `799`
at half the system clock.

It rides along with the pixel comparison rather than standing as its own suite:
every fixture heavy enough to be worth measuring is already one the comparison
boots, and two suites over the same ten images meant loading each of them
twice.

The planes' and sprites' clocks overlap: the sprite stage owns its three
line buffers, erased behind the beam, and runs beside the fills from the
moment its slots decode — the two couple only through port A's rotor, so a
worst line is whichever engine finished last, not a sum.

Read those numbers as a floor. Every fixture stops its 6502 before the
measurement and none of them make sound, so neither takes the port A slot a
running machine would, and no fixture puts a heavy sprite load over mode 3's
XRAM-palette prologue.

## The palette, two ways, and what remains deferred

An earlier version of this section weighed caching a sprite's palette
against fetching it per pixel, gated on width and depth. That question is
settled: the sprite stage carries a sixteen-entry set-associative cache —
eight sets, two ways, one-word lines, flushed each row — sized so any
sixteen contiguous colours are conflict-free, which makes a miss exactly
the fetch the stage was already making. The fill modes deliberately do not
use it: a 256-colour fill's pathological line would miss per pixel and blow
the deadline, and the fill contract is deterministic completion, so fills
keep the whole-palette snapshot in the plane's palram.

What remains deferred is the snapshot's tax: at 8 bpp the reload is a
128-clock prologue on every line, paid even when the palette has not
changed all frame. A reload-skip tag — reload only when the pointer moves
or a write lands inside the palette's range, roughly thirty ALMs of
compare — buys the prologue back. Worth doing only if the render budget
halves; at today's clock the prologue fits with room to spare.

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

