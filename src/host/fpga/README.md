# What FPGA hosts share

The machine itself is in `src/core`: `src/core/sys/wiring.sv` joins its
parts, and `src/core/rtl.cmake` lists its sources and its SDC. An FPGA
host such as `src/host/pocket` puts a board around it. This folder holds
what every FPGA host needs and no board decides:

- `gen/`: the Quartus gates, and `version.py`, the machine's half of the
  register layout version. The gates fail the build on an SDC filter that
  matches no RTL, before a fit, and on a timing, resource, design-rule or
  dropped-constraint problem in a fit's reports.
- `quartus/sta_paths.tcl`: writes the worst paths of every corner beside
  the fit's reports.
- `quartus.cmake`: finds the Quartus tools.

A part moves here when it talks only to the machine, to the soft CPU or
to a seam every FPGA host provides, or when it is a generic Quartus or
build tool. A part that talks to a board resource moves when a second
host needs it.

## The clocks

`src/core/sys/wiring.sdc` constrains the machine but creates no clock.
The host's PLL and SDC do that, and the machine needs three clocks from
one PLL:

- `clk_sys`, 50.4 MHz, period 19.841 ns.
- `clk_a2`, 100.8 MHz, period 9.921 ns, with waveform {8.990 13.951}
  against clk_sys. It clocks XRAM's render port, and its edges fall
  clear of the clk_sys edges the machine's registers change on.
- `clk_ph`, 50.4 MHz, period 19.841 ns, with waveform {12.400 22.321}.
  `xram.sv` samples it as data to tell the two clk_a2 edges apart.

A phase shift is a whole number of eighths of the VCO's period, so the
VCO runs at 1209.6 MHz, where a step is 103.3 ps and both shifts are
whole steps. `src/host/pocket/core/pocket_pll.v` is an example.

## A bitstream runs only the firmware built for it

The register layout version covers the registers and the memory map that
a bitstream and its firmware share. It has three fields, which
`reg_version()` in `gen/version.py` puts together:

| bits | field | written in |
| --- | --- | --- |
| 31-24 | the host's id, the initial of its name | `gen/version.py` here |
| 23-12 | `MACHINE_VERSION`: wiring's decode, soc's 0xF map, `mem_pkg.sv`, and the machine's part of the firmware's `mmio.h` and `link.ld` | `gen/version.py` here |
| 11-0 | the host's own: its host window, its bridge registers and its staging layout | the host's `gen/version.py` |

The host's `gen/version.py` composes the value and makes a package for
the RTL and a header for the firmware. The bitstream shows it at
0x80030000, and `firmware.bin` carries its own in the word after its
reset jump. `ver_check` compares them before the firmware uses any other
register, so that address never moves. When they differ the firmware
stops there and touches nothing more. The 6502 stays in reset and the
screen stays black.

Raise `MACHINE_VERSION` when a machine register the firmware uses moves,
changes meaning or is added, or when the memory map changes. Every host's
version changes with it. Raise a host's own version for the same changes
in what only that host has. A firmware built for one host stops on
another's bitstream on the id alone.
