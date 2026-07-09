# Rumbledethumps' Picocomputer 6502

The Picocomputer 6502 is a real 6502 computer built from a WDC 65C02, a couple
of Raspberry Pi Picos, and very little else. This repository holds everything
that runs on the Picos plus a desktop/web emulator of the whole machine.

The main documentation starts here:<br>
https://picocomputer.github.io/

Pre-built `.uf2` firmware images for Pi Pico 2 boards:<br>
https://github.com/picocomputer/rp6502/releases

This project is for building emulation or firmware. For writing 6502 software, see
[picocomputer/vscode-cc65](https://github.com/picocomputer/vscode-cc65) and
[picocomputer/vscode-llvm-mos](https://github.com/picocomputer/vscode-llvm-mos).

## All Platforms

Begin by installing VS Code and the Pi Pico VS Code Extension as described in
[Getting started with the Raspberry Pi Pico](https://rptl.io/pico-get-started).

Some dependencies are submodules. Don't forget to grab them:
```
$ git submodule update --init
```

The emulator's debugger needs one nested submodule. Don't use `--recursive`,
which downloads much more than needed:
```
$ git -C vendor/cppdap submodule update --init third_party/json
```

The web build also needs the Emscripten SDK, which lives in the `vendor/emsdk`
submodule. Run the VS Code **emsdk: install and activate** task once to fetch and
activate the toolchain into that submodule (a one-time ~270 MB download). The same
thing may be done from the command line (Windows: emsdk.bat) :
```
$ vendor/emsdk/emsdk install latest
$ vendor/emsdk/emsdk activate latest
```

## Linux

The Pi Pico VS Code Extension may need this additional software:
```
$ sudo apt install python3 git tar build-essential gdb-multiarch pkg-config libftdi1-dev libhidapi-hidraw0
```

For the emulator, install GL/X11 dev headers:
```
$ sudo apt install libgl-dev libx11-dev libxi-dev libxcursor-dev
```

## Windows

The Pi Pico VS Code Extension should only need the install from
[Getting started with the Raspberry Pi Pico](https://rptl.io/pico-get-started).

Nobody is working on the emulator for Windows. Unclaimed territory — wander
in and make it yours. The CMake platform branches (D3D11, wingetopt) are
already in place; what remains is the host filesystem layer (`src/emu/host`,
`src/emu/mon/rom.c`), which still speaks POSIX and needs Win32 shims. The
CMake presets use the Ninja generator, so with MSVC configure from an x64
Native Tools prompt.

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
```

Install required tools:

```bash
brew update
brew install cmake ninja pkg-config
```

## Building with CMake and VS Code

The rp6502 and emu project use different CMake models on purpose. The first
thing you need to remember is that F7 builds with the CMake extension settings
and F5 launches a debug session with the Debug settings.

To build for web, make sure you ran **emsdk: install and activate** after the submodule init.
From the CMake side panel select Folder:emu and Configure:WebAssembly.
Pressing F7 builds two bundles. `build/web/html` is the tester: a menu shell
that runs every test ROM. `build/web/itch.io` is a ready-to-publish itch.io
sample that plays one program (`adventure.rp6502` by default) — see
`src/emu/itch.io/README.md` to retarget and deploy it. Either must be delivered
with a web server; use the VS Code live preview extension `ms-vscode.live-server`
or a simple python server to run them.
`python3 -m http.server 8000 --directory build/web/html`

To build firmware, select Folder:rp6502 and Configure:Pico from the CMake side
panel. Select either the Debug or Release variant. You must select the launch
target for debugging here, either rp6502_ria or rp6502_vga. Pressing F7 will
build the firmware. On the Debug side panel, select the "Pico Debug" option that
matches your debugging setup (probably Cortex-Debug), then press F5.

To build the emulator, ensure your seatbelt is fastened and tray tables in their
upright position; we have some bumpy weather ahead. From the CMake side panel
select Folder:emu and Configure:Debug or Configure:Release. On the Debug side
panel you select "Emulator Debug" and press F5. You'll get prompted to select
one of the included test roms to run. You'll also have a binary in build/emulator
which supports the Debug Adapter Protocol (DAP) that you can use with vscode-cc65
and vscode-llvm-mos, or any other IDE thats support DAP.

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
