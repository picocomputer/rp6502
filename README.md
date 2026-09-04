# Rumbledethumps' Picocomputer 6502

The Picocomputer 6502 is a real 6502 computer built from a WDC 65C02, a couple
of Raspberry Pi Picos, and very little else. This repository holds everything
that runs on the Picos plus a desktop/web emulator and FPGA cores of the whole
machine.

The main documentation starts here:<br>
https://picocomputer.github.io/

Pre-built firmware and executables:<br>
https://github.com/picocomputer/rp6502/releases

This project is for building emulation or firmware. For writing 6502 software, see
[picocomputer/vscode-cc65](https://github.com/picocomputer/vscode-cc65), which
builds with either cc65 or llvm-mos and gets its tools from `tools/` here.

## All Platforms

Begin by installing VS Code and the Pi Pico VS Code Extension as described in
[Getting started with the Raspberry Pi Pico](https://rptl.io/pico-get-started).

Most dependencies are submodules, and CMake fetches the ones your build needs
the first time you configure. `-DRP6502_FETCH_SUBMODULES=OFF` leaves `vendor/`
entirely to you.

## Linux

The Pi Pico VS Code Extension may need this additional software:
```
$ sudo apt install python3 git tar build-essential gdb-multiarch pkg-config libftdi1-dev libhidapi-hidraw0
```

For the emulator, install the GL/X11/ALSA dev headers:
```
$ sudo apt install libgl-dev libx11-dev libxi-dev libxcursor-dev libasound2-dev
```

For the FPGA core, install Verilator and the RISC-V toolchain the soft CPU's
firmware is built with. Without these the FPGA tree still configures; it just
registers fewer tests and offers no bitstream.
```
$ sudo apt install verilator gtkwave ninja-build gcc-riscv64-unknown-elf picolibc-riscv64-unknown-elf
```

## Windows

The Pi Pico VS Code Extension should only need the install from
[Getting started with the Raspberry Pi Pico](https://rptl.io/pico-get-started).

For a native x64 emulator build (MSVC + Ninja, no WSL/MSYS2), open an
**x64 Native Tools Command Prompt for VS**, then:

```
cd src\host\sokol
cmake --preset windows-debug
cmake --build --preset windows-debug
```

## MacOs

The Pi Pico VS Code Extension should only need the install from
[Getting started with the Raspberry Pi Pico](https://rptl.io/pico-get-started).

For the emulator, install Xcode command line tools if needed:

```bash
xcode-select --install
```

Install Homebrew if needed:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew update
brew install cmake ninja pkg-config
```

## Building with CMake and VS Code

Every buildable thing is a CMake project root of its own, one per host, and
`.vscode/settings.json` lists them all. Pick one from the CMake side panel's
Folder list; the Configure list is then that root's own presets. F7 builds with
the CMake extension settings and F5 launches a debug session with the Debug
settings.

| Folder | builds | build directory |
| --- | --- | --- |
| `rp6502` (the repository root) | the two Pico firmwares | `build/` |
| `src/host/sokol` | the emulator on Linux, Windows and macOS, and its test suite | `build/sokol/<os>/{debug,release}` |
| `src/host/itch.io` | the itch.io bundle | `build/itch.io` |
| `src/host/sokol/android` | the same machine on Android: the native library, and an `apk` target | `build/android/` |
| `src/host/libretro` | the libretro core, and its test suite | `build/libretro/{debug,release}` |
| `src/host/pocket` | the Analogue Pocket card package | `build/pocket` |
| `tests/rtl` | the verilated machine and its suite | `build/rtl` |

Adding MiSTer is a directory beside `src/host/pocket` and one more line in that
list; nothing else changes.

To build firmware, select Folder:rp6502. Select either the Debug or Release
variant, and `-DPICO_BOARD=pico2` for a RIA without the radio (the Pico
extension's **Switch Board** does the same thing). You must select the launch
target for debugging here: rp6502-vga, and whichever RIA the board selects --
rp6502-ria-w for a radio board, rp6502-ria without. Pressing F7 will
build the firmware. On the Debug side panel, select the "Pico Debug" option that
matches your debugging setup (probably Cortex-Debug), then press F5.

A refactor that was meant to move code and not change it can say so.
`tests/fwsize.py` compares two firmware ELFs function by function, after
undoing what LTO renames, and reports what resized rather than what the
section totals drifted by:

    cp build/ria-w/rp6502-ria-w.elf /tmp/before.elf
    # ...change something...
    python3 tests/fwsize.py --nm ~/.pico-sdk/toolchain/15_2_Rel1/bin/arm-none-eabi-nm \
        /tmp/before.elf build/ria-w/rp6502-ria-w.elf

It exits non-zero when anything moved, so it also works as a gate in a
script. The soft CPU's image is `build/rtl/assets/sw.elf` with
`--nm riscv64-unknown-elf-nm`.

To build the emulator, select Folder:sokol and Configure:Debug or
Configure:Release. On the Debug side panel select
"Emulator Debug" and press F5. You'll get prompted to select one of the
included test roms to run. You'll also have a binary that supports the Debug
Adapter Protocol (DAP) that you can use with vscode-cc65, or any other IDE
that supports DAP.

To build the libretro core, select Folder:libretro and Configure:Debug. On
the Debug side panel select "RetroArch Debug" and press F5; it builds the
core, launches RetroArch on it, and stops at your breakpoints. You'll get
prompted for one of the included test roms, or use "RetroArch Debug
(path…)" to type the path of any ROM you like. RetroArch has to be
installed; on WSL the launch configuration already works around the
missing window decorations.

To build for web, select Folder:itch.io; the Emscripten toolchain installs itself
the first time and needs no preset argument to find. Pressing F7 builds
`build/itch.io/bundle`, a ready-to-publish itch.io sample that plays one program
(`adventure.rp6502` by default) — see `src/host/itch.io/dist/README.txt` to retarget
and deploy it.

`src/host/itch.io/index.html` is the tester: a menu of every test ROM, run against
that same bundle. It stays in the source tree, so serve the repository root
rather than the build. Use the VS Code live preview extension
`ms-vscode.live-server` and open `src/host/itch.io/index.html`, or a simple python
server. Neither page works from a `file://` URL; the browser needs an HTTP
origin to fetch a ROM or stream the wasm.
`python3 -m http.server 8000` then http://localhost:8000/src/host/itch.io/index.html

To build the Pocket core, select Folder:pocket. F7 assembles the SD card tree
into `build/pocket/package`; `pocket-bitstream`, `pocket-fit` and `synth` are
targets in the build-target selector if you want to stop earlier or just
measure. It needs Quartus and `gcc-riscv64-unknown-elf` and nothing else.

## Testing

The suite is in two halves, because they cost wildly different amounts.

The suite is filed by what a test claims, not by which build runs it.

`tests/cpu` is the machine's own — the 6502 and its VIA, the video modes, the
filesystem, HID, the RIA's API. Where a claim can be made of both
implementations it is written once and run against whichever machine the tree
builds, through a seam:

| seam | binds | so one suite covers |
| --- | --- | --- |
| `dut.h` / `cpu_dut.h` | `chips_dut.c`, `w65c02_dut.cpp` | the 6502, per cycle |
| `via_dut.h` | `via_chips.c`, `via_rtl.cpp` | the 6522, per cycle |
| `tests/bench/mut.h` | `mut_emu.c`, `mut_rtl.cpp` | the whole machine — boot a program, take a frame |

Registrations that need a simulator, the shipped binary or the staged assets
guard on those existing, so the same directory serves every tree.

A test that renders carries its own expectation, as a CRC of the settled
frame written into the case:

    UTEST(mode3, two_bpp8_fills_serial_640x480)
    {
        run_case(utest_result, "fill_heavy640", 0x42E2D810, MUT_BUDGET_UNDER);
    }

Both machines answer to that number, so either can be tested without the
other — neither is the other's oracle. A failure prints what it got beside
what it expected; re-blessing a deliberate renderer change is editing the case
that failed, so the expectation and the claim move in one diff. Setting
`RP6502_BLESS_CRC` makes a run print every case's observed value in the form
it is pasted back as.

`tests/host/emu` and `tests/host/pocket` are claims about a program or a
board rather than about the RP6502: the command line, the script channel and
the debugger for one; the bridge, the SDRAM staging and the dock's HID
mapping for the other.

`tests/rtl` is what only a simulator can answer — the beam, the modules below
any mode, pin-scripted lockstep, the soft CPU — and it is also the project
root that builds the verilated machine.

So an emulator build runs its host tests and the machine's; the fpga build
runs its host tests, the machine's, and the RTL's:

    cd src/host/sokol                  # one root for all three desktops
    cmake --preset linux-release     # or macos-release, windows-release
    cmake --build --preset linux-release
    ctest --preset linux-release     # host/emu + cpu, a couple of seconds

    cd tests/rtl                     # needs Verilator, and
    cmake --preset release           # gcc-riscv64-unknown-elf for the soft CPU
    cmake --build --preset release
    ctest --preset release           # host/pocket + cpu + rtl

`ctest -L cpu`, `-L rtl`, `-L host.emu` and `-L host.pocket` pick out a slice
by what is claimed. `-L sim` cuts the other way — everything the simulator
runs, wherever it lives, which is the slow half of the fpga build.

## General Linux and WSL notes

Don't forget that you can get Pi Pico SDK setup help from the
[Raspberry Pi Forums](https://forums.raspberrypi.com/).

Add a udev rule to avoid needing root access for openocd. Create
`/etc/udev/rules.d/99-pico.rules` with:
```
#Raspberry Pi Foundation
SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", MODE="0666"
```

WSL won't start udev by default. Create or edit `/etc/wsl.conf` with:
```
[boot]
command="service udev start"
```

Add your user account to the dialout group so you don't need root for serial
device access:
```
$ sudo usermod -a -G dialout $USER
```

You can forward USB ports to WSL with
[usbipd-win](https://github.com/dorssel/usbipd-win):
```
PS> winget install usbipd
PS> usbipd list

BUSID  VID:PID    DEVICE
7-4    2e8a:000c  CMSIS-DAP v2 Interface, USB Serial Device (COM1)

PS> usbipd attach --wsl --busid 7-4
```

VS Code Serial Monitor doesn't yet send breaks or let you slow down a paste.
Minicom is still useful.
```
$ minicom -w -c on -R cp437 -b 115200 -o -D /dev/ttyACM0
```
