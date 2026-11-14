# The console terminal: work still to do

term.c is a VT102 with most of xterm and the Linux console. This is what is
left, kept here until it is done. Each item says what it is and what it takes.

Two facts shape the work:

- **A cell has no spare bits.** A cell is 8 bytes: the glyph, 8 bits of
  attributes, and the foreground, background and underline colors. All eight
  attribute bits are used. The Pico renderer (core/vga/mode/mode0.c) and the
  Pocket's FPGA both read cells straight from memory, so a change to the cell
  is a change to both renderers, and the FPGA's is an RTL change built with
  Quartus.
- **Keys are made on the RIA; modes live on the VGA.** The keyboard is read
  and turned into bytes on the RIA. term.c, which hears a program switch a
  keyboard or mouse mode, runs on the VGA, so those modes have to get from the
  VGA to the RIA.

## 1. The Pocket's renderer: double-size lines and the reverse screen

DECALN, SL/SR, the reverse screen (DECSCNM) and double-width and double-height
lines are done in term.c and the Pico renderer. The Pocket's software already
passes the two new pieces to the fabric: a row's size (TERM_LINE_) in bits
16-17 of its VID_ROW word, and DECSCNM in bit 26 of VID_CURSOR. mode0.sv still
has to draw them, which needs the next Quartus build:

- **Reverse screen:** swap every cell's foreground and background.
- **Double width:** draw the row's first half of cells, every pixel twice
  across.
- **Double height:** as double width, drawing the top or bottom half of each
  glyph with every glyph row twice.

## 2. Needs bigger cells, and the memory for them

- **Recolor text already on screen** when OSC 4, 104, 10, 11 or the Linux
  palette sequences change a color, as xterm and Linux do. Today only new text
  takes the new color. A cell keeps where its colors came from, and a color
  change starts a background pass, one row a tick, rewriting the cells that use
  it.
- **Protected areas and selective erase.** DECSCA marks characters that the
  selective erases (DECSED, DECSEL) leave alone; ECMA-48's SPA, EPA and ERM do
  the same. Used for DEC form screens. One bit per cell.

**The plan:** make cells bigger rather than keep a separate table, and only go
back to a separate table if this fails. A cell keeps its 8 bytes for the
renderers and gains 29 bits:

- where the foreground, background and underline colors came from: a palette
  entry 0-255, the default, or RGB (9 bits each);
- faint (1 bit), so a recolored color can be halved again;
- protected (1 bit).

That makes a 12-byte cell, which stays word-aligned for the renderers. Both
terminals stay, 40 and 80 columns, each with its alternate screen.

**Explore splitting the cell into two tables** before settling on 12-byte
cells: one with only what the renderers read (the scanout memory, VIDCELLS on
the Pocket), and one with what only term.c needs (CPU RAM). The new bits above
are all term.c-only: the color sources and protected are never drawn. Look too
at what today's 8 bytes hold just because it was simpler to keep it there, and
whether any of it can move out of the scanout memory, which is the scarce one
on the Pocket. If the split is clean, the renderers' cells stay small and the
rest goes to the soft CPU's RAM, which the memory work below frees.

**The Pico VGA** has room: its cells grow from 61 KB to 92 KB, and the video
region can grow down into free RAM and stay in the upper banks.

**The Pocket** does not. Its cells live in FPGA block memory (VIDCELLS, 61,440
bytes), 12-byte cells need 86.4 KB, and the FPGA has only about five M10K
blocks left. The memory has to come from the soft CPU: its 96 KB are M10K
blocks too, and getting it down to 64 KB frees 32 KB for VIDCELLS (an RTL
change).

Today the soft CPU uses 70.6 KB of code, 15.8 KB of data and a 4 KB stack:
90.5 KB. Getting to 64 KB means cutting about 26.5 KB. Measured so far, building
with `ninja sw_bin` only:

| Subsystem | Code | Data |
|---|---|---|
| libc and libgcc | 11.4 KB | |
| term.c | 9.6 KB | 1.3 KB |
| the main loop, with the drivers inlined into it | 7.1 KB | |
| APF file slots | 4.8 KB | |
| API | 4.1 KB | |
| line editor | 4.0 KB | 2.6 KB |
| file system | 3.9 KB | 2.3 KB |
| mode 2 options | | 3.1 KB |

Ideas, with what each would save:

- **Our own time library, the same on every platform,** with only what we use:
  no `strftime` from libc, no `tzset` and the `sscanf` it alone pulls in, no
  `mktime`, `localtime` or `gmtime`, and no `setenv` to pass a timezone. 32-bit
  day arithmetic also drops the 64-bit modulo. About 6 KB.
- **`mode2_options`** is kept for every scanline and plane. If the Pocket only
  needs it once per program, about 3 KB.
- **`api_return_errno`** is a `static inline` in api.h, and the Pocket build
  has eight copies of its 320 bytes. As one function, about 2.2 KB.
- **The line editor's history** serves only the monitor, which the Pocket does
  not have. 2 KB.
- **64-bit divides** in the APF, HID and clock code pull in libgcc's 64-bit
  division. About 1.7 KB.
- **Buffers:** com.c's 1 KB buffer and the file system's pool. 1-2 KB, once
  their sizes are checked.

That is about 16-17 KB. The rest has to come from term.c, the drivers inlined
into the main loop and the APF code, measured as we go.

Speed doesn't matter on the soft CPU: it runs at 50 MHz and mostly copies
bytes. It already builds with `-Os`; `-Oz` gives the same binary with this GCC.
The `O3` pragmas in the mode and audio files cost only about 250 bytes there,
since their renderers are not built for the Pocket. A function that does need
speed can be tagged on its own.

## 3. Keyboard modes: a tighter keyboard interface

- **Cursor-key mode, DECCKM `?1`.** The arrow keys send `ESC O A` instead of
  `ESC [ A`. Full-screen programs such as vim and less turn it on.
- **Keypad modes, DECKPAM `ESC =` and DECKPNM `ESC >`.** The numeric keypad
  sends application sequences instead of digits.
- **LNM's keyboard half.** With LNM set, Return sends CR LF. The screen half is
  done.
- **Autorepeat, DECARM `?8`.** Off stops held keys repeating.
- **Keyboard LEDs, DECLL `CSI q`.** Lights the keyboard's LEDs.

All of these need the modes to get from term.c on the VGA to the keyboard on
the RIA: a message over the PIX bus or the backchannel, or the RIA picking
these few sequences out of the output stream itself. That design comes first.
The line editor already reads the keys these modes produce.

Testing: vttest's keyboard tests need their keys to go through the same path
that turns a real keyboard's keys into bytes (core/hid/vtkeys.c), so the modes
can change them. The vttest harness (tests/host/vttest/vtrun.c) writes keys
straight to vttest today, and would type through vtkeys instead.

## 4. Mouse

- **Mouse reporting,** the xterm modes `?9` and `?1000` to `?1006`, and maybe
  the DEC locator from the VT330/340. A program asks for clicks, drags or
  motion, and the terminal sends them as escape sequences. Which protocols to
  support is decided when this starts. The mode is set on the VGA and the mouse
  is on the RIA, so the design for the keyboard modes should carry the mouse
  modes too.
- Once `?1001` exists, `CSI 1;x;y;10;20 T` has to be told apart from SD.
- Selecting text on the screen locally is a separate feature, which we don't
  have.
