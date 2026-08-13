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
[picocomputer/vscode-cc65](https://github.com/picocomputer/vscode-cc65) and
[picocomputer/vscode-llvm-mos](https://github.com/picocomputer/vscode-llvm-mos).

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
cd src\emu
cmake --preset debug
cmake --build --preset debug
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

The rp6502 and emu project use different CMake models on purpose. The first
thing you need to remember is that F7 builds with the CMake extension settings
and F5 launches a debug session with the Debug settings.

To build for web, select Folder:emu and Configure:WebAssembly from the CMake
side panel; the toolchain installs itself the first time.
Pressing F7 builds two bundles. `build/web/html` is the tester: a menu shell
that runs every test ROM. `build/web/itch.io` is a ready-to-publish itch.io
sample that plays one program (`adventure.rp6502` by default) — see
`src/dist/itch.io/README.md` to retarget and deploy it. Either must be delivered
with a web server; use the VS Code live preview extension `ms-vscode.live-server`
or a simple python server to run them.
`python3 -m http.server 8000 --directory build/web/html`

To build firmware, select Folder:rp6502 and Configure:Pico from the CMake side
panel. Select either the Debug or Release variant. You must select the launch
target for debugging here, either rp6502-ria or rp6502-vga. Pressing F7 will
build the firmware. On the Debug side panel, select the "Pico Debug" option that
matches your debugging setup (probably Cortex-Debug), then press F5.

To build the emulator, from the CMake side panel
select Folder:emu and Configure:Debug or Configure:Release. On the Debug side
panel you select "Emulator Debug" and press F5. You'll get prompted to select
one of the included test roms to run. You'll also have a binary in build/emulator
which supports the Debug Adapter Protocol (DAP) that you can use with vscode-cc65
and vscode-llvm-mos, or any other IDE thats support DAP.

To build the FPGA core, select Folder:rtl. Its Configure list is one entry per
job, each with a build directory of its own:

    verilator/Release   the simulation and its tests
    verilator/Debug     the same, unoptimised, for stepping a testbench
    pocket              the Analogue Pocket core
    quartus             area and timing, all pins virtual

Pick one and the Build list changes with it — under `pocket` it offers
**Card package** and **Bitstream**, under the Verilator presets **Tests** and
**Firmware**. F7 builds whichever is selected. Naming every host explicitly is
so MiSTer can arrive without renaming anything.

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
