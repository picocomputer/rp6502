# Rumbledethumps' Picocomputer 6502

The Picocomputer explores retro computing and game development by removing the barrier between genuine 8-bit hardware and modern devices. It can be built entirely with through-hole components, compactly using surface mount devices, or even on a breadboard. No programming devices need to be purchased and the only component used that wasn't available in the 1980s is the $4/€4 Raspberry Pi Pico.

Learn how it works on YouTube:<br>
https://www.youtube.com/@rumbledethumps

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

## Hardware Notes

The PIX port will likely be "jumpered" into a high-speed graphics bus. However, work on this hasn't started so I left these pins exposed in case someone comes up with a better use for it.

The Pico VGA is optional. Something to control the RIA over the UART Tx/Rx lines is still required. You might, for example, be developing a video system based on other hardware and prefer to have your video chip control the RIA.

The 6522 is optional. You may need to pull-up IRQB/VIRQ if you omit this from the reference design. The GPIOs are not used for anything, but some software may not function without timers.

The only thing not optional is putting the Pico RIA at FFE0-FFFF. This reference design is very capable, but also very easy to build on a breadboard. Feel free to change the glue circuit to support paged RAM, parallel ROMs, expansion slots, and DIP switches. Those are exactly the expense and complexity the Pico RIA replaces, but you should not encounter any resistance if you want to experiment and learn about them.

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
