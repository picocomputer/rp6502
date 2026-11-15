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

## 3. Keyboard LEDs and locks

For review. Nothing here is built.

### How the LED command works

`CSI Ps q` (DECLL) is a VT100 command for four spare lamps on its keyboard,
L1 to L4, that a program could light for any reason. 0 puts them all out and
1 to 4 light one; several numbers in one command are done left to right.
xterm uses it for a PC's lamps, 1 Num, 2 Caps, 3 Scroll, and adds 21 to 23
to put one out. Linux numbers them differently and lights one at a time.

It only ever lights a lamp. It never changes a lock, so what the keys type
stays the same and a lamp can disagree with its lock. Most terminals today
ignore it, nothing can read it back, and vttest only asks a person to look at
the keyboard.

### What each machine can do

A **lock** is what the keys type (NumLock, Caps Lock, Scroll Lock). A
**lamp** is the light.

| Machine | Read the locks | Change the locks | Light a lamp alone |
|---|---|---|---|
| Pico | owns them | owns them | yes |
| Pocket | owns them | owns them | no way found to reach the dock keyboard's lamps |
| Windows | all three | all three, for the whole PC | no |
| Linux, real Xorg | Caps, Num | Caps, Num, for the whole desktop | Scroll only |
| Linux, Wayland desktops and WSL | Caps, Num | no | no |
| macOS | Caps; it has no Num or Scroll | Caps, for the whole Mac (needs a test on a Mac) | no |
| Web | when a key is pressed; Safari only Caps | no | no |
| libretro | in key events, if the frontend sends them | no | no |

Three things follow:

- **No emulator can do what DECLL does.** Where an emulator can change
  anything it changes the real lock, for every program on the computer, and
  the web, Wayland and libretro can change nothing.
- **An emulator never needs to change a lock.** A lock key pressed in its
  window is the computer's own key and has already changed the lock. The only
  other thing that would is DECLL, which is not meant to.
- **Scroll Lock cannot follow the computer** on Linux, macOS or Safari.

### Plan

1. **Lamps apart from locks, in the core.** One byte of lamps beside the
   locks. A lock change, RIS and the reset at a program's end make the lamps
   show the locks. DECLL lights and puts out lamps with xterm's numbers and
   leaves the locks alone. The 6502 keeps seeing the locks.
2. **The Pico** sends the lamps to the keyboard where it sends the locks now.
   The Pocket does the same once there is a way to light its dock keyboard.
3. **The emulators follow the computer and never change it.** One function
   for each of Windows, Linux, macOS and the web says which locks the
   computer has and what they are; the emulator reads it at start, when its
   window gets the keyboard, and while it runs. A lock the computer does not
   have (Num and Scroll on a Mac, Scroll on Linux) is the emulator's own,
   toggled by its key, with NumLock on at start. The keypad then follows
   NumLock in the window as it does on the Pico. DECLL changes the lamps byte
   and nothing a person can see.
4. **libretro** follows the frontend once the frontend has shown that it
   sends locks, and until then keeps its own, toggled by their keys. Several
   RetroArch input drivers send none, and its Mac driver sends wrong ones.
5. **The script has its own locks**, all off when a script starts, never
   read from or written to a real keyboard. `lock` sets one on or off or
   toggles it, `key` types by them (NumLock for the keypad, Caps Lock for
   letters), and a new command checks the locks and the lamps. Scripts that
   type keypad digits turn NumLock on first.
6. **Pull request 223** has the Windows read, the place for the function and
   the moments to read. Its waiting for the computer's character to guess
   NumLock goes, since the lock is read. It is written for main and has to be
   redone here.

The Pocket's cost is step 1 alone: DECLL in the parser and one byte. A guess
is 200 bytes; it is measured when built.

### To settle

- **Emulators never change the computer's locks** (step 3). The other choice
  is to change them where it can be done: Windows, real Xorg and Caps Lock on
  a Mac. Then a 6502 program sending DECLL turns on the Caps Lock of the
  whole computer, and the web, most Linux desktops and libretro still do
  nothing.
- **Who holds a lamp DECLL lit.** Step 1 gives it back at the next lock key,
  as X does. Linux keeps DECLL's lamps until a reset.

### To try on real machines

- Windows: whether a lock changed while the window is in the background is
  seen before the window is back in front.
- macOS: reading Caps Lock, and whether Sokol needs help to see its key.
- Whether a held lock key repeats in the window and flips the lock.
- A Pocket dock: whether anything lights a keyboard's lamps.
