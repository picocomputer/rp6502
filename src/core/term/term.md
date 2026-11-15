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

## 3. The terminal on the RIA, and the keyboard modes

The RIA follows the VGA's terminal and answers the queries (step 2, done):
ria/term/cells.c is the RIA's terminal, ria/sys/vga.c replays the VGA's size
changes and resets, 0x0D turns the answers off, and 0x0A and 0x0B give a
program's own size as a third grid (rln.c). Pinning either axis fixes both:
the other takes the screen's size at that moment.

### Step 3: the line editor stops probing (done)

The line editor asks no terminal anything. A read takes the prompt column,
width and height from the RIA's terminal once everything printed has reached
it; rln breaks its own rows with CR LF and moves between them with CUU and
CUD, and a new width starts the line over on a clean row. Cursor shapes go to
the screen alone, NAWS is refused, and "cursor in column 1" is the clean line.
Headless with stdout a pipe or file, the line editor types plainly
(rln_set_plain).

### Step 4: keyboard modes (done)

The terminal holds the keyboard's modes: cursor keys `?1`, the keypad
(`ESC =`, `ESC >`, `?66`), autorepeat `?8`, LNM and VT52. vtkey.c turns a key
into bytes for every machine and is the only reader of them; keymap.c (RIA,
Pocket) and hostkey.c (emulators) type through it. Libretro has no repeat
flag, so a press of a key already down is its repeat.

The keys send what xterm sends, checked key by key against xterm with every
modifier in every mode. The differences are on purpose:

- **The keypad.** NumLock on types the characters. NumLock off is Insert and
  the arrows, or a VT100's keypad when a program asked for it (`ESC =`), with
  no Shift needed. Shift with NumLock on is NumLock off, with the Shift used
  up; with NumLock off Shift, Ctrl and Alt are modifiers. Plus is the VT100's
  comma, as on the Linux console, and so is the keypad comma some keyboards
  have; divide and multiply send xterm's codes. F1 to F4 are PF1 to PF4, so
  the keypad sends none of them.
- **The GUI key** changes nothing a key sends. GUI-Space cycles the layout.
- **VT52 mode** changes only a VT52's keys. Alt still puts ESC in front, and
  the keys a VT52 lacks send what they always do.

tio frames `ESC ? x` as one key in VT52 mode. The line editor's simple mode
sends no escape sequences and is used for a read that starts in VT52 mode,
insert mode is suspended in VT52 mode, and `ESC A-D`, `ESC H` and `ESC F` are
always the arrows, Home and End. The soft reset puts the cursor keys, keypad
and autorepeat back. vttest's keyboard menu runs, typed through hostkey.

## 4. Mouse

After section 3. xterm's `?9` and `?1000` to `?1006`, maybe the DEC locator;
which ones is decided then. Its modes are more terminal state on the RIA.
Once `?1001` exists, `CSI 1;x;y;10;20 T` has to be told apart from SD.

## 5. Keyboard LEDs and NumLock on the emulators

DECLL (`CSI q`) lights a VT100's L1 to L4, which xterm maps to Num, Caps and
Scroll Lock (1, 2, 3; 21 to 23 put one out). The lock keys already use those
lights, so the two have to share them. The emulators (sokol, libretro, web)
and the Pocket don't drive or report keyboard LEDs yet, so that host work
comes first. Deferred until then.

The same host work covers NumLock: Sokol reports none, so its keypad always
acts as with NumLock off.

The emulator script is part of this. Its `lock num|caps|scroll` toggles a
lock, but `key` does not look at the locks: it always types with NumLock on
and takes Caps Lock from nowhere. To do with the LEDs:

- **`key` follows the locks.** NumLock and Caps Lock as `lock` left them
  decide what a key types, as on the RIA, so a script reaches the keypad with
  NumLock off, and Shift there is a modifier.
- **A script sets a lock** on or off, not only toggles it.
- **A script reads the LEDs**, the three locks and what DECLL lit, to check
  them the way it checks the screen.
