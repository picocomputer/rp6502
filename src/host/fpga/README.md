# What FPGA hosts share

The machine itself is in `src/core`: `src/core/sys/wiring.sv` joins its
parts, and `src/core/rtl.cmake` lists its sources and its SDC. An FPGA
host such as `src/host/pocket` puts a board around it. This folder holds
what every FPGA host needs and no board decides:

- `gen/`: the Quartus gates, and `version.py`, the machine's half of the
  register layout version. The gates fail the build on an SDC filter that
  matches no RTL, before a fit, and on a timing, resource, design-rule or
  dropped-constraint problem in a fit's reports, on an assignment that
  reaches nothing, and on a source file read from outside a project that
  must build from its own folder. They have been run on the reports of
  Quartus 17.0.2 and 25.1; CI fits the Pocket with 21.1.
- `quartus/sta_paths.tcl`: writes the worst paths of every corner beside
  the fit's reports, and the names report: how many registers each of
  the machine SDC's collections matches, and how many nodes each instance
  assignment's target does.
- `quartus.cmake`: two functions. `rp6502_quartus_tools()` finds one
  Quartus's tools, from a folder a host hints at or the PATH, and can
  insist on a version. `rp6502_quartus_gates()` compiles a host's project
  with the commands it gives, a stock flow or one in steps, and runs every
  gate on the reports. Its comment lists them; a host gives the device's
  limits and its baselines: the design-rule findings it accepts, the size
  of each SDC collection, and the assignments it cannot avoid leaving
  unapplied.
- `sw/`: the soft CPU's firmware, all but the host's own units. `mmio.h`
  is the machine's map as the soft CPU sees it, and `link.ld` places the
  image.
- `sw.cmake`: `rp6502_fpga_sw()`, which builds the firmware from `sw/`,
  core and the units a host names.

A part moves here when it talks only to the machine, to the soft CPU or
to a seam every FPGA host provides, or when it is a generic Quartus or
build tool. A part that talks to a board resource moves when a second
host needs it.

## The host seam

A host's `sw.cmake` calls `rp6502_fpga_sw()` with its folder as `DIR`,
its own units as `SOURCES` and its generated headers as `DEPENDS`. The
host's units come first in the link, ahead of the shared ones: the order
steers LTO's inlining, and this one makes the smaller image. `DIR` is the
first `-I`, so three headers are found there by bare name:

- `machine.h` and `drivers.h`, as on every host;
- `fpga_host.h`, what the shared firmware asks of the host's registers.

`crt0.S` takes one thing from `fpga_host.h`: `FPGA_REG_VERSION`, the
register layout version it carries. For C the header gives four
`static inline` functions, so the seam costs nothing:

| function | gives |
| --- | --- |
| `bool fpga_rtc_valid(void)` | whether the host has given a time |
| `uint32_t fpga_rtc_epoch(void)` | that time: local, in seconds since 1970 |
| `int32_t fpga_tz_minutes(void)` | the UTC offset in minutes, east positive |
| `int32_t fpga_kb_layout(void)` | a position in `def/keyboard.def` plus one, 0 for none yet |

`sw/time.c` makes the wall time from the first three, and `sw/cfg.c`
follows the zone and the layout as the host's settings change. A host
that leaves one out fails to compile. The host's own units implement
the rest of what core asks of a host that `sw/` does not: its
filesystem, its process glue, its input driver's `hid_set_leds`,
`hid_boot_enumerating` and `hid_remapped`, and `main`.
`src/host/pocket` is the example.

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

A phase shift is a whole number of eighths of the VCO's period. Both
shifts are whole eighths of 2480 ps, the period of 403.2 MHz, 8 times
clk_sys: 8990 ps is 29 and 12400 ps is 40. So the VCO runs at a whole
multiple of 403.2 MHz. `src/host/pocket/core/pocket_pll.v` is an
example: from the Pocket's 74.25 MHz it runs at 806.4 MHz, where a step
is 155 ps.

## A bitstream runs only the firmware built for it

The register layout version covers the registers and the memory map that
a bitstream and its firmware share. It has three fields, which
`reg_version()` in `gen/version.py` puts together:

| bits | field | written in |
| --- | --- | --- |
| 31-24 | the host's id, the initial of its name | `gen/version.py` here |
| 23-12 | `MACHINE_VERSION`: wiring's decode, soc's 0xF map, `mem_pkg.sv`, `sw/mmio.h` and `sw/link.ld` | `gen/version.py` here |
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
