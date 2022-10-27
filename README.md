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

## Hello World
Online assembler:
https://www.masswerk.at/6502/assembler.html
```
.org $0200
LDX #$00
loop:
LDA text,X
INX
STA $FFEE ; UART Tx
CMP #$00
BNE loop
STA $FFEF ; Halt 6502
text:
.ASCII "Hello, World!"
.BYTE $0D $0A $00
```
Once you have machine code, copy-paste it and jmp.
```
0200: A2 00 BD 10 02 E8 8D EE
0208: FF C9 00 D0 F5 8D EF FF
0210: 48 65 6C 6C 6F 2C 20 57
0218: 6F 72 6C 64 21 0D 0A 00
JMP $200
```

## Pi Pico - Power, CDC, and PicoProbe.
| #Pins | Description
| -  | -
| 17 | VGA - 640x480 32768 colors
| 2  | STDIO UART
| 2  | SWD
| 1  | PHI2
| 1  | RESB
| 3  | PBUS

## Pi Pico - USB Host for Hub.
| GP#   | Description
| ----- | -
|  0    | Chip Select
|  1    | Write Enable
|  2-9  | Data D0-D7
| 10-14 | Address A0-A4
| 15    | Bank A16
| 16-17 | STDIO UART
| 18-19 | PWM Audio L-R
| 20    | IRQB
| 21    | PHI2
| 22    | RESB
| 26-28 | PBUS
