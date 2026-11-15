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

## 2. Harvest memory

Memory that comes back on every machine. The Pocket is the one that needs
it: its soft CPU has 96 KB and about 7 KB free, and section 4 needs about
14 KB more. Sizes are measured in the Pocket's image.

- **Options in the scanline table.** Mode 2 keeps the options word of its
  XREG call in an array of its own, for every scanline and plane: 3 KB that
  the Pocket never reads, since its FPGA table already holds the options. A
  row of the scanline table gets an options word for each plane, beside the
  renderer and the config pointer. Mode 2 reads it from the row and
  `mode2_options` goes. A savestate reads it from the row too, so the
  functions in modes 0, 1 and 3 that work the options out from the renderer
  go. 3 KB on the Pocket, about 1 KB on the Pico. Check mode 2's speed on
  the board.
- **64-bit divides.** The four library routines are 3.5 KB together, and
  one goes when nothing calls it. The callers: the PCM file open, the HID
  analog scaling, the time of day, the clock attribute, the APF slot task
  and the system walk. libc's `gmtime_r` calls the two signed ones, so
  those 1.8 KB wait for section 3.
- **The line editor's history** is 2 KB, 8 lines of 256 bytes, and serves
  only the monitor, which the Pocket doesn't have. A flag turns it off as
  the program runs, so the linker keeps it. The monitor brings the history,
  and a machine without a monitor doesn't link it.

Staying: the 1 KB scratch buffer, and the file pool's 2.2 KB, whose names
reopen the files after a savestate.

- **One scratch buffer, in core.** Every machine has one 1 KB buffer that
  the ROM reader fills a record at a time, but each brings its own: the RIA
  passes the monitor's `mbuf`, and the emulators and the Pocket each have a
  `static buf` with no name inside their loader. Core gets the one buffer,
  `buf1k`, in core/sys/buf1k.c, and every machine links it. The ROM reader
  reads into it and its functions lose their buffer argument, the two
  unnamed buffers go, and the RIA's `mbuf` becomes `buf1k`. What the RIA's
  monitor keeps beside it stays in the RIA: how many bytes it holds, and
  reading it from the console. No memory changes.

## 3. Our own time library

On every platform, with only what we use: no `strftime`, `tzset` (and the
`sscanf` it pulls in), `mktime`, `localtime`, `gmtime` or `setenv`; 32-bit
day arithmetic. About 6 KB on the Pocket, not measured again, and the last
1.8 KB of the 64-bit divides.

## 4. Recolor and protected areas

After sections 2 and 3 free the memory.

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

## 5. The lock keys, to try on real machines

The emulators follow the system's NumLock, Caps Lock and Scroll Lock. The
Windows and macOS code has been compiled by CI and never run, and the web
build has not been tried in a browser.

- Windows: whether a lock changed while the window is in the background is
  seen before the window is back in front.
- Windows: Shift with a keypad digit while NumLock is on. Windows hides the
  Shift from the key, so the encoder may see a plain digit and, with no
  character coming, type nothing.
- macOS: reading Caps Lock, and whether Sokol needs help to see its key.
- Web: the locks in each browser, and the first key after the page loads.
- A Pocket dock: whether its keyboard's lights follow the lock keys. Nothing
  found so far lets the Pocket light them.
