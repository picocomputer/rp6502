# The console terminal: work still to do

The terminal is three files: term.c (the parser and everything the same at
every size), grid.c (the terminal at one size: cursor, margins, tabs, line
sizes) and cells.c (the screen's cells and their bookkeeping, for a machine
that draws). cells.c writes what mode 0 draws from, which
core/vga/mode/mode0.h defines. This is what is left, in the order to build
it.

Memory is no longer short. The Pocket's soft CPU now runs from SDRAM at
50.4 MHz: its RAM block is 64 KB, of which the firmware uses 16 KB and the
heap has 45 KB free, and 192 KB more sits unused in its memory window for
the RAM block to grow into. Harvesting memory everywhere moved to
src/core/harvest.md, to be done on its own branch after this.

## 1. A second cell table in cells.c

Recolor and protected areas both need more per cell than the 8 bytes the
renderers read. cells.c gets a second table, 3 bytes a cell in CPU RAM,
indexed like mode0_cells (core/vga/mode/mode0.h): the foreground's
and background's source (palette entry, default or RGB, 9 bits each),
faint, protected, and whether the underline follows the foreground. The
renderers and the FPGA don't change. It is filled wherever a cell is
written, scrolled, erased or copied, and a savestate carries it.

| | Renderer table | cells.c table |
|---|---|---|
| Pico VGA, 7,680 cells | 61.4 KB in VIDEO, as now | 23 KB in lower RAM (86 KB free) |
| Pocket, 7,200 cells | 57.6 KB in VIDCELLS, as now | 21.6 KB of the RAM block's 45 KB free |

## 2. Recolor text already on screen

When OSC 4, 104, 10, 11 or the Linux palette sequences change a color,
every cell drawn with that color takes the new one, as xterm and Linux do.
The second table says which cells came from which entry.

## 3. Protected areas and selective erase

DECSCA with DECSED and DECSEL; ECMA-48's SPA, EPA and ERM. The second
table's protected bit says which cells an erase leaves alone.

## 4. The lock keys, to try on real machines

The emulators follow the system's NumLock, Caps Lock and Scroll Lock. The
Windows and macOS code has been compiled by CI and never run, and the web
build has not been tried in a browser. This needs the owner at those
machines.

- Windows: whether a lock changed while the window is in the background is
  seen before the window is back in front.
- Windows: Shift with a keypad digit while NumLock is on. Windows hides the
  Shift from the key, so the encoder may see a plain digit and, with no
  character coming, type nothing.
- macOS: reading Caps Lock, and whether Sokol needs help to see its key.
- Web: the locks in each browser, and the first key after the page loads.
- A Pocket dock: whether its keyboard's lights follow the lock keys. Nothing
  found so far lets the Pocket light them.
