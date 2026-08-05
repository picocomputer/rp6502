# RP6502 for Analogue Pocket

A 6502 computer — the Picocomputer RP6502 — running in the Pocket's
FPGA. Copy the `Assets`, `Cores` and `Platforms` folders to the root of
your SD card, keeping the folders they are in.

Programs are `.rp6502` files. Put them in
`Assets/rp6502/common/` and load one from the core's menu.

Documentation and source: [picocomputer.github.io](https://picocomputer.github.io)

## Credits and licences

The core is BSD-3-Clause. Two vendored parts ship inside the bitstream
rather than beside it, so their notices belong here:

- **OPL2 (YM3812)** — [gtaylormb/opl2_fpga](https://github.com/gtaylormb/opl2_fpga),
  Greg Taylor, LGPL-3.0. A reverse-engineered SystemVerilog OPL2 built
  down from his OPL3 core, itself descended from Robson Cozendey's Java
  OPL3 and Steffen Ohrendorf's C++ port.
- **Hazard3** — [Wren6991/Hazard3](https://github.com/Wren6991/Hazard3),
  Luke Wren, Apache-2.0. The RP2350's own RISC-V core, running the
  machine's real firmware inside the fabric.

The Analogue Pocket framework files under `vendor/openfpga` are
Analogue's core template, used under its own terms.
