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
