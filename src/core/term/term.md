# The console terminal: work still to do

The terminal is three files: term.c (the parser and everything the same at
every size), grid.c (the terminal at one size: cursor, margins, tabs, line
sizes) and cells.c (the screen's cells, for a machine that draws). This is
what is left.

## 1. The Pocket's renderer: double-size lines and the reverse screen

The Pocket's software already passes a row's size in bits 16-17 of VID_ROW
and DECSCNM in bit 26 of VID_CURSOR. mode0.sv has to draw them (Quartus):

- **Reverse screen:** swap every cell's foreground and background.
- **Double width:** draw the row's first half of cells, every pixel twice
  across.
- **Double height:** as double width, drawing the top or bottom half of each
  glyph with every glyph row twice.

## 2. Recolor and protected areas, and the Pocket memory for them

- **Recolor text already on screen** when OSC 4, 104, 10, 11 or the Linux
  palette sequences change a color, as xterm and Linux do.
- **Protected areas and selective erase:** DECSCA with DECSED and DECSEL;
  ECMA-48's SPA, EPA and ERM.

Both need more per cell than the 8 bytes the renderers read. The plan is a
second table in cells.c, 3 bytes a cell in CPU RAM, indexed by the same
physical row: the foreground's and background's source (palette entry,
default or RGB, 9 bits each), faint, protected, and whether the underline
follows the foreground. The renderers and the FPGA don't change.

| | Renderer table | cells.c table |
|---|---|---|
| Pico VGA, 7,680 cells | 61.4 KB in VIDEO, as now | 23 KB in lower RAM (86 KB free) |
| Pocket, 7,200 cells | 57.6 KB in VIDCELLS, as now | 21.6 KB of the soft CPU's 96 KB |

The Pocket needs about 14 KB more free than the ~7 KB it has. Ideas, each
with what it saves:

- **Our own time library** on every platform, with only what we use: no
  `strftime`, `tzset` (and the `sscanf` it pulls in), `mktime`, `localtime`,
  `gmtime` or `setenv`; 32-bit day arithmetic. About 6 KB.
- **`mode2_options`** once per program instead of per scanline and plane.
  About 3 KB.
- **`api_return_errno`** as one function instead of eight inline copies.
  About 2.2 KB.
- **The line editor's history** serves only the monitor, which the Pocket
  doesn't have. 2 KB.
- **64-bit divides** in the APF, HID and clock code. About 1.7 KB.
- **Buffers:** com.c's 1 KB buffer and the file system's pool. 1-2 KB.

## 3. The lock keys on every machine

For review. Nothing here is built.

NumLock, Caps Lock and Scroll Lock have to work on every machine. On the
Pico and the Pocket we are the keyboard driver: a lock key changes the lock
and its light, and that works today. On an emulator the computer's operating
system does both, and the emulator has to follow it. Today no emulator does,
except libretro in part.

The terminal's LED command, DECLL (`CSI Ps q`), stays ignored. It lights a
lamp without changing a lock, which no emulator can do.

### Who owns each lock and its light

"Us" is the machine's own keyboard code: a press of the lock key flips the
lock. "The system" is the operating system the emulator runs on: it flips
the lock and lights the LED, and the emulator reads the lock from it.

| Machine | Lock | Who owns the lock | Who lights the LED |
|---|---|---|---|
| Pico, USB and Bluetooth | all three | us | us. Works today. |
| Pocket | all three | us. Works today. | nobody. The Pocket has no known way to reach the dock keyboard. |
| Windows | all three | the system | the system |
| Linux | Num, Caps | the system | the system |
| Linux | Scroll | us. Linux has no Scroll Lock to read. | nobody |
| macOS | Caps | the system | the system |
| macOS | Num, Scroll | us. A Mac has neither. | nobody |
| Web | all three | the system, read when a key is pressed | the system |
| Web on Safari | Num, Scroll | us. Safari reports only Caps. | the system, for its own lock |
| libretro | all three | the frontend, when it sends them | the system |
| libretro, frontend sends none | all three | us | the system, for its own lock |

A lock of ours on an emulator starts with NumLock on and the others off. No
emulator lights an LED: the few that could would change the lock for the
whole computer or work on one kind of Linux desktop only. So where the lock
is ours and the system has one too (Safari, a libretro frontend that sends
none), the keyboard's light shows the system's lock and can disagree with
ours.

### Plan

1. **The emulators follow the operating system and never change it.** One
   function for each of Windows, Linux, macOS and the web says which locks
   the system has and what they are. The emulator reads it at start, when its
   window gets the keyboard, and while it runs. The 6502 sees those locks,
   and the keypad follows NumLock in the window as it does on the Pico.
2. **libretro** follows the frontend once the frontend has shown that it
   sends locks, and until then keeps its own. Several RetroArch input drivers
   send none, and its Mac driver sends wrong ones.
3. **The script has its own locks**, all off when a script starts, never
   read from or written to a real keyboard. `lock` sets one on or off or
   toggles it, `key` types by them (NumLock for the keypad, Caps Lock for
   letters), and a new command checks them. Scripts that type keypad digits
   turn NumLock on first.
4. **Pull request 223** has the Windows read, the place for the function and
   the moments to read. Its waiting for the system's character to guess
   NumLock goes, since the lock is read. It is written for main and has to be
   redone here.

The Pico and the Pocket do not change, so this costs the Pocket no memory.

### To try on real machines

- Windows: whether a lock changed while the window is in the background is
  seen before the window is back in front.
- macOS: reading Caps Lock, and whether Sokol needs help to see its key.
- Whether a held lock key repeats in the window and flips a lock of our own.
- A Pocket dock: whether its keyboard's lights follow the lock keys. Nothing
  found so far lets the Pocket light them.
