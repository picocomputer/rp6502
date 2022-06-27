# Rumbledethumps' Picocomputer 6502

The RP6502 is an 8-bit computer with VGA, Audio, and USB. It can be built entirely with through-hole components, compactly using surface mount devices, or even on a breadboard. No programming devices need to be purchased and the only component used that wasn't available in the 1980s is a $4/€4 Raspberry Pi Pico.

Join me on YouTube as I develop this project.
https://www.youtube.com/channel/UC7kMjECYMyiSgFfiAm6c55w


## Dev Setup
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

## Pi Pico - USB Host for Hub.
| Pins | Description
| - | -
| 2 | STDIO UART
| 17 | VGA - 640x480 32768 colors
| 1 | PWM Audio
| 1 | PHI2
| 1 | RESB
| 4 | PBUS

## Pi Pico - Power, USB CDC, and PicoProbe.
| Pins | Description
| - | -
| 2 | STDIO UART
| 2 | SWD
| 8 | Data
| 5 | Address
| 1 | CS0
| 1 | RWB
| 1 | IRQB
| 1 | PHI2
| 1 | RESB
| 4 | PBUS

## Memory Map

| Addr | Description
| - | -
| FFFE-FFFF | BRK/IRQB
| FFFC-FFFD | RESB
| FFFA-FFFB | NMIB
| FFF0-FFF9 | 10 bytes for fast load
| FFE0-FFEF | 16 bytes for I/O TBD

## Fast Load
Self modifying RAM avoids some indirection. It's not much, so this may not
happen if the resources are needed elsewhere, but it's worth trying for.
```
.org FFF0
loop:
LDA #00
STA $0000
BRA loop
JMP $0000
```
