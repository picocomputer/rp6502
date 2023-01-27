# Rumbledethumps' Picocomputer 6502

The Picocomputer explores retro computing and game development by removing the barrier between genuine 8-bit hardware and modern devices. It can be built entirely with through-hole components, compactly using surface mount devices, or even on a breadboard. No programming devices need to be purchased and the only component used that wasn't available in the 1980s is the $4/€4 Raspberry Pi Pico.

This is the reference design. It is a 6502 with 64K of SRAM and an optional 6522. A pair of Raspberry Pi Picos are used for power, clocking, video, audio, WiFi, and USB. It is programmed over USB. A filesystem is planned so it can be programmed with a USB flash drive.

Learn how it works on YouTube:<br>
https://youtube.com/playlist?list=PLvCRDUYedILfHDoD57Yj8BAXNmNJLVM2r

Connect with other 6502 homebrew enthusiasts on Discord:<br>
https://discord.gg/TC6X8kTr6d

## Memory Map

| Addr | Description
| - | -
| FFFE-FFFF | BRK/IRQB Vector
| FFFC-FFFD | RESB Vector
| FFFA-FFFB | NMIB Vector
| FFF0-FFF9 | 10 bytes for fast load
| FFE0-FFEF | 16 bytes for I/O TBD
| FF10-FFDF | Unallocated for Expansion
| FF00-FF0F | 6522 VIA
| 0200-FEFF | Free RAM
| 0100-01FF | Stack Page
| 0000-00FF | Zero Page

Preliminary I/O registers (beta)<br>
FFE0 - UART status, bit 7 Tx ready, bit 6 Rx ready<br>
FFE1 - UART Tx<br>
FFE2 - UART Rx<br>
FFEF - Write anything here to stop 6502<br>

## Project Status

Hardware is tested. Schematic for the reference design is unlikely to change.

Graphics Mode 0, aka Color ANSI Terminal, is working. USB keyboard works. EhBASIC works.

Addressable-memory graphics modes are not yet implemented.

Sound is not yet implemented.

Filesystem is not yet implemented.

## Hardware Notes

The PIX port will likely be "jumpered" into a high-speed graphics bus. However, work on this hasn't started so I left these pins exposed in case someone comes up with a better use for it.

The Pico VGA is optional. Something to control the RIA over the UART Tx/Rx lines is still required. You might, for example, be developing a video system based on other hardware and prefer to have your video chip control the RIA.

The 6522 is optional. You may need to pull-up IRQB/VIRQ if you omit this from the reference design. The GPIOs are not used for anything, but some software may not function without timers.

## Dev Setup

This is only for building the Pi Pico software. For writing 6502 software, see the examples.

Install the C/C++ toolchain for the Raspberry Pi Pico. For more information, read [Getting started with the Raspberry Pi Pico](https://rptl.io/pico-get-started).
```
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential gdb-multiarch
```

All dependencies are submodules. The following will download the correct version of all SDKs. It will take an extremely long time to recurse the project, so do this instead:
```
git submodule update --init
cd src/pico-sdk
git submodule update --init
cd ../pico-extras
git submodule update --init
cd ../..
```

Only build with Release or RelWithDebInfo. Debug builds may not have a fast enough action loop. This will be addressed later.

The Pi Pico VGA is also a Picoprobe for development and terminal use. Load a release build of rp6502_vga.uf2 on it with the BOOT SEL button. You can do the UF2 process with the RIA board too, or use the VGA/Picoprobe board to load it using OpenOCD.

The VSCode launch settings connect to a remote debug session. I use two terminals for the debugger and monitor. You'll also want to add a udev rule to avoid a sudo nightmare. The following are rough notes, you may need to install software which is beyond the scope of this README.

Create `/etc/udev/rules.d/99-pico.rules` with:
```
#Picoprobe
SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", MODE="0666"
```
Debug terminal:
```
$ openocd -f interface/picoprobe.cfg -f target/rp2040.cfg -s tcl
```
Monitor terminal:
```
$ minicom -c on -b 115200 -o -D /dev/ttyACM0
```
WSL (Windows Subsystem for Linux) can forward the Picoprobe to Linux:
```
PS> usbipd list
BUSID  DEVICE
7-4    USB Serial Device (COM6), Picoprobe

PS> usbipd wsl attach --busid 7-4
```
WSL needs udev started. Create `/etc/wsl.conf` with:
```
[boot]
command="service udev start"
```
