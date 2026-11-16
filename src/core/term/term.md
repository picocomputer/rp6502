# The console terminal: work still to do

The terminal is three files: term.c (the parser and everything the same at
every size), grid.c (the terminal at one size: cursor, margins, tabs, line
sizes) and cells.c (the screen's cells and their bookkeeping, for a machine
that draws). cells.c writes what mode 0 draws from, which
core/vga/mode/mode0.h defines. This is what is left, in the order to build
it.

Memory is no longer short. The Pocket's soft CPU now runs from SDRAM at
50.4 MHz: its RAM block is 64 KB, of which the firmware uses 38 KB, 21.6 KB
of it the table of where each cell's colors came from, and 192 KB more sits
unused in its memory window for the RAM block to grow into. Harvesting memory everywhere moved to
src/core/harvest.md, to be done on its own branch after this.

## 1. The lock keys, to try on real machines

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
