# The machine, in both languages

`src/core` is the Picocomputer's parts. Most of them exist twice — once in C for
the machines that run in software, once in SystemVerilog for the machine that
runs as fabric — and the two are meant to be recognisable as the same thing.

This file is the convention that makes them so.

## Naming

**A file is named for the thing it is.** The directory supplies the rest, so a
file never repeats its directory: `vga/mode0.sv`, not `vga/vid_mode0.sv`.

**The same thing has the same name in both languages.** `wdc/cpu.c` and
`wdc/cpu.sv` are the same part. Where only one language has it, the name still
says what it is rather than which language reached it.

**Nothing is named for a part number.** The machine has a CPU and a VIA; WDC
sells a W65C02 and a W65C22. Where a vendor owns a prefix we cannot take it —
the `chips` model owns `w65c02_*` inside `cpu.c`, which is exactly why our own
functions there are `cpu_*`.

Two exceptions, both because a name has to survive without its directory:

- **SystemVerilog module and package names are one global namespace**, shared
  with everything under `vendor/`, which already claims `timer`, `operator`,
  `channels` and `i2s`. A module cannot lean on its directory the way a filename
  can, so where the plain word is taken it carries a qualifier — `soc_bus`, not
  `bus`, when `wdc/bus.sv` already exists.
- **Generated artifacts land flat.** Everything `gen/` emits arrives in one
  assets directory, where `rom_pkg.sv` and `coef_pkg.sv` beside each other say
  nothing. Generated files keep a qualifier; only the source tree drops one.

## The RTL's port convention

**Outputs carry the module's name. Inputs do not.**

    module fill (
        input  logic       start,      // in: bare
        input  logic [7:0] attr_i,
        output logic       fill_done,  // out: prefixed
        output logic       fill_px_we
    );

So a connection reads source-to-sink — `.fill_done(done)` — and a signal's name
says where it came from. This is not redundancy, and it means renaming a module
renames its outputs with it. `mem/sram64k.sv` is the one file that does not
follow it.

Instance names follow their module in `src/core`, because the Verilator
testbenches build their `__DOT__` paths out of both. Under `src/host` an
instance is a short noun for whatever the board is wrapping.
