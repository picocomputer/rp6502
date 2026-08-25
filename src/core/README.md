# The machine

Everything that makes a Picocomputer a Picocomputer, and nothing about the
thing running it. What is here is shared by every machine the repository
builds: two Pico firmwares, an emulator on four desktops and the web, a
libretro core, and an FPGA core on the Analogue Pocket.

## The contracts

The headers at the root of this directory are what a machine must answer:
`cfg.h` `com.h` `cpu.h` `main.h` `mem.h` `pix.h`. Each has three
implementations — `core/sys` for a machine made of software, `host/pico/ria`
for the RP2350 firmware, `host/pocket/sw` for the Hazard3 soft CPU — and none
of them is the reference. What a *host* owes the machine is the other
direction and lives in `src/host`: `os.h`, `fs.h`, `dir.h`.

## The subsystems

A subsystem keeps its C and its SystemVerilog together, because they are two
implementations of one claim and the tests hold them to it:

    api/      the 6502 syscall ABI
    aud/      the bell, the PSG, the OPL2, the resampler
    def/      X-macro data: keyboard layouts, localized strings
    hid/      keyboards, mice, gamepads, tablets, and the report parser
    machine/  the machine's RTL top, and its timing constraints
    mem/ ria/ rv/    RAM, the register window, the soft RISC-V SoC
    str/ term/       readline and the ANSI terminal
    vga/      the video modes, as renderers and as fabric
    wdc/      the 6502 and the VIA

## The software machine

`core/sys` is what the fabric does in fabric and a firmware does in silicon,
written in C: the bus and clock engine, the RIA, the video and audio devices,
the drives, the debug seam. It is a peer of `host/pico/ria` and
`host/pocket/sw`, not a layer above them.

`core/dap` is the debug adapter and the DWARF/cc65 readers behind it — machine
debugging, independent of any window. The interface that draws it belongs to
whoever has a screen, and is `src/host/sokol`.

## The parts lists

    emu.cmake      emu_core: the software machine as a library
    machine.cmake  the machine as RTL plus the soft CPU's firmware
    assets.cmake   the tables generated from the sources above
    quartus.cmake  where Quartus is, and the machine's own constraints
    synth.cmake    the machine through synthesis, for area and timing

`core/gen` holds the generators those modules run. Nothing they emit is
committed. The roots that include these modules are in `src/mach`, one per
machine, and `tests/rtl` — which is also where the simulation and the FPGA
work are documented.
