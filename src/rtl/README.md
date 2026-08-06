# RP6502 FPGA core

The Picocomputer 6502 as an FPGA core. The Analogue Pocket (openFPGA) is the
first target; MiSTer is planned. Both are Cyclone V, so `rtl/` stays platform
independent and each host gets a thin wrapper under `platform/`.

The 6502, VIA, video renderers and audio are RTL. A Hazard3 soft RISC-V runs a
trimmed build of the real `src/ria` firmware C for the OS layer — syscalls, HID
and ROM loading — mirroring the RP2350 + W65C02 split of the real machine.

## Audio

The PSG is RTL and agrees with `ria/aud/psg.c` sample for sample in lockstep.
Its ninth voice is the console bell, configured by the soft CPU: the sounds
are `ria/aud/bel_presets.c` and the queue and lifetime are
`src/fpga/sw/bel.c`, so fabric holds a voice and software holds the bell.

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

## Layout

    rtl/core/       the machine, independent of the FPGA platform
    platform/       per-host wrappers (Pocket APF, MiSTer), Quartus projects

Tests live with every other test, in `tests/`, filed by subsystem rather than
by host: the verilated `aud_psg` is in `tests/aud` beside the `psg.c` it is
held to, not in a directory of its own. `tests/bench` carries the Verilator
testbench and the emu_core reference oracle; `tests/host/pocket` carries what
is genuinely about this board rather than about the machine.

## Building the simulation

Development is simulation first. Tests run the same `.rp6502` on the verilated
machine and on `emu_core`, then compare — the emulator is the reference for
behavior the RTL must reproduce.

    sudo apt-get install verilator gtkwave ninja-build
    cd src/fpga
    cmake --preset fpga/verilator/Release
    cmake --build --preset Tests
    ctest --preset fpga/verilator/Release

Ninja is required, not preferred, and CMake stops if it is missing: the
pocket testbench builds one verilated model that two tests link, which
make will build twice at once and then link half-written or stale.

## The four trees

`CMakePresets.json` has one entry per job this source tree can do, and each
is a build directory of its own. What varies is whether the verilated
machine and its tests are part of it — `RP6502_FPGA_SIM`.

| preset | builds in | what it is for |
| --- | --- | --- |
| `fpga/verilator/Release` | `build/fpga/release` | the simulation and the whole suite |
| `fpga/verilator/Debug` | `build/fpga/debug` | the same, unoptimised, for stepping a testbench |
| `fpga/pocket` | `build/fpga/pocket` | the Analogue Pocket core |
| `fpga/quartus/synth` | `build/fpga/synth` | area and timing, all pins virtual |

**A bitstream needs Quartus and `gcc-riscv64-unknown-elf`, and nothing
else.** No Verilator, no host test suite. That was not true for a while:
the Quartus projects were built from a source list defined inside a
`verilator_FOUND` guard, so a machine without a simulator had no bitstream
target at all — including the CI runner whose whole job is fitting one. The
list moved to `rtl.cmake`, which every configuration includes, and it is
still the list the simulation verilates, so the thing measured is still the
thing tested.

Without Verilator only the oracle tests build; CMake warns and continues.

Set `RP6502_FPGA_TRACE` to a path to capture an FST trace, viewable in gtkwave:

    RP6502_FPGA_TRACE=rtl.fst ./build/fpga/release/tests/test_rtl

## Measuring the render budget

`test_budget` counts, for every scanline of the heaviest fixtures, the clocks
from the line boundary to the last engine going idle, and reports XRAM port A's
occupancy beside it. The beam's deadline is pixel 799 — the next line's pixel
zero is read then — so a line is `2 * 799` clocks today and would be `799`
at half the system clock.

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

