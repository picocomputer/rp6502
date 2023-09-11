# Rumbledethumps' Picocomputer 6502

The Picocomputer explores retro computing and game development by removing the barrier between genuine 8-bit hardware and modern devices. It can be built entirely with through-hole components, compactly using surface mount devices, or even on a breadboard. No programming devices need to be purchased and the only component used that wasn't available in the 1980s is the $4/€4 Raspberry Pi Pico.

Learn how it works on YouTube:<br>
https://www.youtube.com/@rumbledethumps

Read the documentation:<br>
https://picocomputer.github.io/

## Dev Setup

This is only for building the Pi Pico software. For writing 6502 software, see the [rp6502-vscode](https://github.com/picocomputer/rp6502-vscode) and [rp6502-sdk](https://github.com/picocomputer/rp6502-sdk).

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

The Pi Pico VGA is also a Picoprobe for development and terminal use. Load a release build of rp6502_vga.uf2 on it with the BOOT SEL button. You can do the UF2 process with the RIA board too, or use the VGA/Picoprobe board to load it using OpenOCD.

The VSCode launch settings connect to a remote debug session. I use two terminals for the debugger and monitor. You'll also want to add a udev rule to avoid a sudo nightmare. The following are rough notes, you may need to install software which is beyond the scope of this README.

Create `/etc/udev/rules.d/99-pico.rules` with:
```
#Picoprobe
SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", MODE="0666"
```
Debug terminal: (use OpenOCD 0.11)
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
