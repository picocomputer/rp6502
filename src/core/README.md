# The machine

Everything that makes a Picocomputer a Picocomputer, and nothing about the
thing running it. What is here is shared by every machine the repository
builds: two Pico firmwares, an emulator on four desktops and the web, a
libretro core, and an FPGA core on the Analogue Pocket.

## The contracts

The headers at the root of this directory are what a machine must answer:
`cfg.h` `com.h` `cpu.h` `main.h` `mem.h` `pix.h` `sys.h`. Three machines
answer them — the software one whose parts are filed by subsystem below,
`host/pico/ria` for the RP2350 firmware, `host/pocket/sw` for the Hazard3
soft CPU — and none of the three is the reference. Where two of them would
answer the same way, one implementation here answers for both and they
supply the difference.

`main.h` is the one all three now share outright: starting and stopping the
6502 is a request, `main.c` holds the state it is a request against, and each
machine supplies only its own fan-outs and the moment it can afford to run
them.

What a *host* owes the machine is the other direction and lives in
`src/host`: `os.h`, `fs.h`, `dir.h`.

## The subsystems

A subsystem keeps its C and its SystemVerilog together, because they are two
implementations of one claim and the tests hold them to it:

    api/      the 6502 syscall ABI, and the launcher chain behind it
    aud/      the bell, the PSG, the OPL2, the resampler, the mixer
    com/      the console: two rings, a bell, and what a Ctrl-C means
    dap/      the debug adapter, the DWARF and cc65 readers, the engine
    def/      X-macro data: keyboard layouts, localized strings
    hid/      keyboards, mice, gamepads, tablets, the report parser, and
              what a key spells on the wire
    machine/  the machine's RTL top, and its timing constraints
    mem/ ria/ rv/    RAM, the register window, the soft RISC-V SoC
    str/ term/       readline and the ANSI terminal
    sys/      what a machine has rather than what it is made of: the frame
              and bus engine, the PIX bus, the XREG rows, the drives, the
              ROM format, the clock, the log, the version
    vga/      the video modes and the scanline program, as renderers and
              as fabric
    wdc/      the 6502 and the VIA, as software and as fabric

## One implementation, or a good reason

A machine is not a fork. Where one implementation can serve every machine it
lives here and each machine answers the few calls that are genuinely its own:
the console is `com/`, over the wire in `com/tty.h`; the launcher chain is
`api/proc.c`, over how a machine starts a program; the scanline program is
`vga/prog.c`, over what a machine's canvas is. The 6502's own ABI is the
clearest case, because none of it was ever a machine's to choose: which
handler a syscall reaches is `api/ops.c`, the eighteen directory calls are
`api/dir.c` over a drive, and the XREG rows are `sys/main_xreg.c`.

Where a machine is genuinely different it says so and keeps its own — the
RIA's console arbitrates a real serial port against a keyboard and a network
socket, and does not fit above that seam.

What this buys is not lines. A copy drifts: the Pocket lost Ctrl-C for as
long as its console was a copy of the emulator's, because the copy was taken
before the latch existed and nobody diffed them again. A machine cannot lose
a feature it does not implement.

The interface that draws a debugger belongs to whoever has a screen, and is
`src/host/sokol`; what it draws is `dap/`.

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
