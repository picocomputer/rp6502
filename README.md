# Rumbledethumps' Picocomputer 6502

The Picocomputer explores retro computing and game development by bridging the void between genuine 8-bit hardware and modern devices. It can be built entirely with through-hole components, compactly using surface mount devices, or even on a breadboard. No programming devices need to be purchased and the only component used that wasn't available in the 1980s is the $5/€5 Raspberry Pi Pico 2.

The main documentation starts here:<br>
https://picocomputer.github.io/

## Dev Setup

This is for building the Pi Pico software. For writing 6502 software, see [picocomputer/vscode-cc65](https://github.com/picocomputer/vscode-cc65).

Begin by installing VSCode and the Pi Pico VSCode Extension as described in [Getting started with the Raspberry Pi Pico](https://rptl.io/pico-get-started).

Some dependencies are submodules. Don't forget to grab them:
```
$ git submodule update --init
```

This is all you would need to do in an ideal world. But the Pi Pico tools run on many operating systems which makes documentation a moving target. They assume you can fill in some blanks. The following are my notes for setting up WSL (Windows Subsystem for Linux) with Ubuntu. Don't forget that you can get help from the [Raspberry Pi Forums](https://forums.raspberrypi.com/).

The Pi Pico VSCode Extension will need this additional software:
```
$ sudo apt install build-essential gdb-multiarch pkg-config libftdi1-dev libhidapi-hidraw0
```

Add a udev rule to avoid needing root access for openocd. Create `/etc/udev/rules.d/99-pico.rules` with:
```
#Raspberry Pi Foundation
SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", MODE="0666"
```

WSL won't start udev by default. Create or edit `/etc/wsl.conf` with:
```
[boot]
command="service udev start"
```

Add your user account to the dialout group so you don't need root for serial device access:
```
$ sudo usermod -a -G dialout $USER
```

You can forward USB ports to WSL with [usbipd-win](https://github.com/dorssel/usbipd-win):
```
PS> winget install usbipd
PS> usbipd list

BUSID  VID:PID    DEVICE
7-4    2e8a:000c  CMSIS-DAP v2 Interface, USB Serial Device (COM1)

PS> usbipd attach --wsl --busid 7-4
```
