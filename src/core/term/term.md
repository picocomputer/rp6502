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

NumLock, Caps Lock and Scroll Lock have to work on every machine. On the
Pico and the Pocket we are the keyboard driver: a lock key changes the lock
and its light. On an emulator the computer's operating system does both, and
the emulator follows it.

The terminal's LED command, DECLL (`CSI Ps q`), stays ignored. It lights a
lamp without changing a lock, which no emulator can do.

### Who owns each lock

A lock and its light stay together: whoever owns the lock lights its LED.
"Us" is the machine's own keyboard code, where a press of the lock key
flips the lock. "The system" is the operating system the emulator runs on,
which flips the lock and lights the LED; the emulator reads the lock and
never changes it.

| Machine | Lock | Owner |
|---|---|---|
| Pico, USB and Bluetooth | all three | us. Works today. |
| Pocket | all three | us. The lock works today; the LED does not light, and the Pocket has no known way to reach the dock keyboard. |
| Windows | all three | the system |
| Linux | Num, Caps | the system |
| Linux | Scroll | nobody: Linux has no Scroll Lock. Always off. |
| macOS | Caps | the system |
| macOS | Num | nobody: a Mac has no NumLock and its keypad always types digits. Always on. |
| macOS | Scroll | nobody. Always off. |
| Web | all three | the system, read when a key is pressed |
| Web on Safari | Num, Scroll | as on a Mac: Safari reports only Caps |
| libretro | all three | the system, through what the frontend sends with a key |

A lock that nobody owns never changes. Its key still shows in the keyboard
bitmap, so a program can use the key.

### What is built

- **The owner of the locks sets them.** keyboard.c only keeps the locks, for
  the program's bitmap and for what the keys type, and they start off.
  keymap.c is the keyboard driver of the Pico and the Pocket: it starts with
  NumLock on, a lock key flips its lock, and it lights the LEDs. An emulator
  has none of that; its host passes in the system's locks.
- **The key encoder reads NumLock** from keyboard.c. No caller passes it.
- **The window follows the system and never changes it.** `host_locks()`
  in each of windows, linux, macos and web `entry.c` gives the three locks,
  and `sokol/app/input.c` reads it at start, once a frame and as a key goes
  down. A lock key only shows in the bitmap.
  - Windows: `GetKeyState`.
  - Linux: `XkbGetState`, Caps from the Lock modifier and Num from the
    modifier the keymap gives the Num_Lock key.
  - macOS: `[NSEvent modifierFlags]`.
  - Web: `page.js` keeps what `getModifierState` said in the last key or
    click. Before the first one NumLock is taken as on. On an Apple device
    Num is always on and Scroll always off, in any browser.
- **libretro** takes the locks the frontend sends with a key. A frontend
  that sends none leaves all three off.
- **A script** has no system under it, so its locks stay off until `lock`
  sets them.
- **One US keyboard** in hostkey.c types a key the system gives no text
  for: a Ctrl or Alt chord in the window or libretro, and every key of a
  script. The window, libretro, the script and the vttest walk had a table
  of their own before.

### To try on real machines

The Windows and macOS code has not been compiled or run. Linux was run
under WSLg and the web build only compiled.

- Windows: whether a lock changed while the window is in the background is
  seen before the window is back in front.
- Windows: Shift with a keypad digit while NumLock is on. Windows hides the
  Shift from the key, so the encoder may see a plain digit and, with no
  character coming, type nothing.
- macOS: reading Caps Lock, and whether Sokol needs help to see its key.
- Web: the locks in each browser, and the first key after the page loads.
- A Pocket dock: whether its keyboard's lights follow the lock keys. Nothing
  found so far lets the Pocket light them.
