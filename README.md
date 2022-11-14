# Rumbledethumps' Picocomputer 6502

The Picocomputer explores retro computing and game development by removing the barrier between genuine 8-bit hardware and modern devices. It can be built entirely with through-hole components, compactly using surface mount devices, or even on a breadboard. No programming devices need to be purchased and the only component used that wasn't available in the 1980s is the $4/€4 Raspberry Pi Pico.

This is the reference design. It is a 6502 with 64K of SRAM and an optional 6522. A pair of Raspberry Pi Picos are used for power, clocking, video, audio, WiFi, and USB. It is programmed over USB. A filesystem is planned so it can be programmed with a USB flash drive.

Learn how it works on YouTube:<br>
https://youtube.com/playlist?list=PLvCRDUYedILfHDoD57Yj8BAXNmNJLVM2r

Connect with other 6502 homebrew enthusiasts on Discord:<br>
https://discord.gg/TC6X8kTr6d

## Dev Setup
Standard Raspberry Pi Pico C SDK setup per the official docs; set PICO_SDK_PATH and PICO_EXTRAS_PATH, use cmake. The VSCode launch settings connect to a remote debug session. For example, I use a Picoprobe under WSL by passing the device to Linux with usbipd-win then bringing up a couple of terminals for the debugger and monitor.

The Pi Pico VGA is also a Picoprobe for development and terminal use. Load a release build of rp6502_vga.uf2 on it with the BOOT SEL button. You can do the UF2 process with the RIA board too, or use the VGA/Picoprobe board to load it using OpenOCD.

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

## Examples
Online assembler:
https://www.masswerk.at/6502/assembler.html
```
.org $0200 ; Hello, World!
LDX #$00   ; X = 0
loop:
LDA text,X ; A = text[X]
STA $FFEE  ; UART Tx A
INX        ; X = X + 1
CMP #$00   ; if A - 0 ...
BNE loop   ; ... != 0 goto loop
STA $FFEF  ; Halt 6502
text:
.ASCII "Hello, World!"
.BYTE $0D $0A $00
```
```
0200: A2 00 BD 10 02 8D EE FF
0208: E8 C9 00 D0 F5 8D EF FF
0210: 48 65 6C 6C 6F 2C 20 57
0218: 6F 72 6C 64 21 0D 0A 00
JMP $200
```

```
* = $0200 ; 6522 Blink
VIA_DDRA = $FF03
VIA_ORA  = $FF01
LDA #$FF
STA VIA_DDRA
loop:
LDA #$00
STA VIA_ORA
JSR delay
LDA #$FF
STA VIA_ORA
JSR delay
JMP loop
delay:
DEY
BNE delay
DEX
BNE delay
RTS
```
```
0200: A9 FF 8D 03 FF A9 00 8D
0208: 01 FF 20 18 02 A9 FF 8D
0210: 01 FF 20 18 02 4C 05 02
0218: 88 D0 FD CA D0 FA 60
jmp $200
```
