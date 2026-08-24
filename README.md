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
cd src\host\win
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

Every buildable thing is a CMake project root of its own, one per host, and
`.vscode/settings.json` lists them all. Pick one from the CMake side panel's
Folder list; the Configure list is then that root's own presets. F7 builds with
the CMake extension settings and F5 launches a debug session with the Debug
settings.

| Folder | builds | build directory |
| --- | --- | --- |
| `rp6502` (the repository root) | the two Pico firmwares | `build/` |
| `src/host/linux` | the emulator on Linux, and its test suite | `build/linux/{debug,release}` |
| `src/host/win` | the same on Windows | `build/win/{debug,release}` |
| `src/host/macos` | the same on macOS | `build/macos/{debug,release}` |
| `src/host/web` | the itch.io bundle | `build/web` |
| `src/host/android` | the native library, and an `apk` target | `build/android/` |
| `src/host/pocket` | the Analogue Pocket card package | `build/pocket` |
| `tests/rtl` | the verilated machine and its suite | `build/rtl` |

Adding MiSTer is a directory beside `src/host/pocket` and one more line in that
list; nothing else changes.

To build firmware, select Folder:rp6502. Select either the Debug or Release
variant, and `-DPICO_BOARD=pico2` for a RIA without the radio (the Pico
extension's **Switch Board** does the same thing). You must select the launch
target for debugging here, either rp6502-ria or rp6502-vga. Pressing F7 will
build the firmware. On the Debug side panel, select the "Pico Debug" option that
matches your debugging setup (probably Cortex-Debug), then press F5.

To build the emulator, select the folder for your platform and
Configure:Debug or Configure:Release. On the Debug side panel select
"Emulator Debug" and press F5. You'll get prompted to select one of the
included test roms to run. You'll also have a binary that supports the Debug
Adapter Protocol (DAP) that you can use with vscode-cc65, or any other IDE
that supports DAP.

To build for web, select Folder:web; the Emscripten toolchain installs itself
the first time and needs no preset argument to find. Pressing F7 builds
`build/web/bundle`, a ready-to-publish itch.io sample that plays one program
(`adventure.rp6502` by default) — see `src/dist/itch.io/README.txt` to retarget
and deploy it.

`src/host/web/index.html` is the tester: a menu of every test ROM, run against
that same bundle. It stays in the source tree, so serve the repository root
rather than the build. Use the VS Code live preview extension
`ms-vscode.live-server` and open `src/host/web/index.html`, or a simple python
server. Neither page works from a `file://` URL; the browser needs an HTTP
origin to fetch a ROM or stream the wasm.
`python3 -m http.server 8000` then http://localhost:8000/src/host/web/index.html

To build the Pocket core, select Folder:pocket. F7 assembles the SD card tree
into `build/pocket/package`; `pocket-bitstream`, `pocket-fit` and `synth` are
targets in the build-target selector if you want to stop earlier or just
measure. It needs Quartus and `gcc-riscv64-unknown-elf` and nothing else.

## Testing

The suite is in two halves, because they cost wildly different amounts.

`tests/emu` is the C and script tests. It rides in each emulator root's build,
so building any of the three above builds it, and `ctest` in that build
directory runs it in a couple of seconds. Nothing in it needs Verilator.

`tests/rtl` is the verilated machine — the RTL modules, the whole-machine
comparisons against the emulator as the reference oracle, and the Pocket's own
bridge. It is a project root of its own, so it costs nothing until you open it,
and it needs Verilator plus `gcc-riscv64-unknown-elf` for the soft CPU. Where a
claim is about both machines it is made from both halves against one set of
evidence: `tests/rtl/wdc` compiles the corpora and the DUT interface out of
`tests/emu/wdc` rather than keeping a second copy of them.

    cd tests/rtl
    cmake --preset release
    cmake --build --preset release
    ctest --preset release

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
