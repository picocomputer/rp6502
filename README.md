# Rumbledethumps' Picocomputer 6502

The RP6502 is an 8-bit computer with VGA, Audio, and USB. It can be built entirely with through-hole components, compactly using surface mount devices, or even on a breadboard. No programming devices need to be purchased and the only component used that wasn't available in the 1980s is a $4/€4 Raspberry Pi Pico.

Join me on YouTube as I develop this project.
https://www.youtube.com/channel/UC7kMjECYMyiSgFfiAm6c55w


## Dev Setup Notes

Standard Raspberry Pi Pico C SDK setup per the official docs; set PICO_SDK_PATH and PICO_EXTRAS_PATH, use cmake. The VSCode launch settings connect to a remote debug session. For example, I use a PicoProbe under WSL by passing the device to Linux with usbipd-win then bringing up a couple of terminals for the debugger and monitor.

```
PS> usbipd list
BUSID  DEVICE
7-4    USB Serial Device (COM6), Picoprobe

PS> usbipd wsl attach --busid 7-4
```
```
$ sudo openocd -f interface/picoprobe.cfg -f target/rp2040.cfg -s tcl
```
```
$ sudo minicom -c on -b 115200 -o -D /dev/ttyACM0
```
