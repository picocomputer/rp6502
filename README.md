# Rumbledethumps' Picocomputer 6502

The Picocomputer 6502 is a real 6502 computer built from a WDC 65C02, a couple
of Raspberry Pi Picos, and very little else. This repository holds everything
that runs on the Picos plus a desktop/web emulator of the whole machine.

The main documentation starts here:<br>
https://picocomputer.github.io/

Pre-built `.uf2` firmware images for Pi Pico 2 boards:<br>
https://github.com/picocomputer/rp6502/releases

## Layout

This repository is **two** CMake projects that share the `src/` and `vendor/`
trees:

* **Firmware** — the repository root itself is the RP6502-RIA, RP6502-RIA-W, and
  RP6502-VGA firmware for the Pico 2 boards, built with the Pico SDK
  ([CMakeLists.txt](CMakeLists.txt)).
* **Emulator** — [src/emu/](src/emu/) is a desktop and WebAssembly emulator of
  the machine (CPU + RIA + VGA) that shares the firmware's own rendering and
  audio code. See [src/emu/README.md](src/emu/README.md) to build it.
* [src/](src/) — shared source: the firmware (`ria/`, `vga/`) and the emulator
  (`emu/`).
* [vendor/](vendor/) — third-party dependencies. Most are git submodules; run
  `git submodule update --init` to populate them.

## Opening in VS Code

Open the repository **root** as the folder — not a `.code-workspace`. The root
*is* the firmware project, so the Raspberry Pi Pico extension activates the way
it expects, and CMake Tools lists both the firmware (root) and the emulator
(`src/emu`). Switch between them in the CMake Tools sidebar (or the status-bar
picker), then configure / build / debug the selected project. The shared `src/`
and `vendor/` are right there in the same window.

Select the **firmware** project before any Pico action (compile / flash /
on-chip debug); select the **emulator** project to build or debug the
desktop/web build.

The two projects use different CMake models on purpose: the firmware builds
through the Pico **kit** + build-type variant (the extension requires it), while
the emulator uses its own **CMakePresets** (`debug` / `release` / `wasm`). Build
the selected project with `F7`. For the emulator, `F5` offers `Debug emu (pick
ROM)` (choose from `tests/roms`), `Debug emu (dir --fs)`, and `Debug emu (prompt
ROM + args)`.

## Firmware Dev Setup

This is for building the firmware. For writing 6502 software, see
[picocomputer/vscode-cc65](https://github.com/picocomputer/vscode-cc65) and
[picocomputer/vscode-llvm-mos](https://github.com/picocomputer/vscode-llvm-mos).

Begin by installing VS Code and the Pi Pico VS Code Extension as described in
[Getting started with the Raspberry Pi Pico](https://rptl.io/pico-get-started).

Some dependencies are submodules. Don't forget to grab them:
```
$ git submodule update --init
```

This is all you would need to do in an ideal world. But the Pi Pico tools run on
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
