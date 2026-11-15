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

### What each emulator can read

| Emulator | From the operating system | Its own, toggled by the key |
|---|---|---|
| Windows | Num, Caps, Scroll | |
| Linux | Num, Caps | Scroll |
| macOS | Caps | Num, Scroll |
| Web | Num, Caps, Scroll when a key is pressed; Safari only Caps | on Safari, Num and Scroll |
| libretro | what the frontend sends in key events | all three, when the frontend sends none |

A lock of its own starts with NumLock on and the others off.

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
