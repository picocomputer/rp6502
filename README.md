# Rumbledethumps' Picocomputer 6502

The Picocomputer 6502 is a real 6502 computer built from a WDC 65C02, a couple
of Raspberry Pi Picos, and very little else. This repository holds everything
that runs on the Picos plus a desktop/web emulator of the whole machine.

The main documentation starts here:<br>
https://picocomputer.github.io/

Pre-built `.uf2` firmware images for Pi Pico 2 boards:<br>
https://github.com/picocomputer/rp6502/releases


## Developer Tools Setup

This is for building emulation or firmware. For writing 6502 software, see
[picocomputer/vscode-cc65](https://github.com/picocomputer/vscode-cc65) and
[picocomputer/vscode-llvm-mos](https://github.com/picocomputer/vscode-llvm-mos).

Begin by installing VS Code and the Pi Pico VS Code Extension as described in
[Getting started with the Raspberry Pi Pico](https://rptl.io/pico-get-started).

Some dependencies are submodules. Don't forget to grab them:
```
$ git submodule update --init
```
The web build also needs the Emscripten SDK, which lives in the `vendor/emsdk`
submodule. Run the VS Code **emsdk: install and activate** task once to fetch and
activate the toolchain into that submodule (a one-time ~270 MB download). The same
thing from the command line:
```
$ vendor/emsdk/emsdk install latest   # Windows: vendor\emsdk\emsdk.bat
$ vendor/emsdk/emsdk activate latest
```

## CMake with VS Code

The rp6502 and emu project use different CMake models on purpose. The first
thing you need to remember is that F7 builds with the CMake extension settings
in the side panel and F5 launches a debug session with the Debug settings in
the side panel.

To build for web, make sure you ran **emsdk: install and activate** after the submodule init.
From the CMake side panel select Folder:emu and Configure:WebAssembly.
Pressing F7 will build a test bundle in src/emu/build/web which must be
delivered with a web server. You can use the VS Code live preview extension
`ms-vscode.live-server` or a simple python server to run the example.
`python3 -m http.server 8000 --directory src/emu/build/web`

To build firmware, select Folder:rp6502 and Configure:Pico from the CMake side
panel. Select either the Debug or Release variant. You must select the launch
target for debugging here, either rp6502_ria or rp6502_vga. Pressing F7 will
build the firmware. On the Debug side panel, select the "Pico Debug" option that
matches your debugging setup (probably Cortex-Debug), then press F5.

To build the emulator, ensure your seatbelt is fastened and tray tables in their
upright position; we have some bumpy weather ahead. From the CMake side panel
select Folder:emu and Configure:Debug or Configure:Release. On the Debug side
panel you select "Emulator Debug" and press F5. You'll get prompted to select
one of the included test roms to run. You'll also have a binary in src/emu/build
which supports the Debug Adapter Protocol (DAP) that you can use with vscode-cc65
and vscode-llvm-mos, or any other IDE thats support DAP.

## Additional Firmware Dev Setup

The above is all you would need to do in an ideal world. But the Pi Pico tools run on
many operating systems which makes documentation a moving target. The following
are my notes for setting up WSL (Windows Subsystem for Linux) with Ubuntu. Don't
forget that you can get help from the
[Raspberry Pi Forums](https://forums.raspberrypi.com/).

The Pi Pico VS Code Extension will need this additional software:
```
$ sudo apt install build-essential gdb-multiarch pkg-config libftdi1-dev libhidapi-hidraw0
```

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

## License

BSD 3-Clause. See [LICENSE](LICENSE); every source file carries an
`SPDX-License-Identifier`. The emulator additionally bundles third-party code
under permissive licenses — see
[src/emu/THIRD-PARTY-NOTICES.md](src/emu/THIRD-PARTY-NOTICES.md).
