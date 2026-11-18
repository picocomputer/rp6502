# The console: the plan

The console is three files: term.c (the parser, and everything that is
the same at every size), grid.c (the console at one size: cursor,
margins, tabs, and line sizes), and cells.c (the cells and their
bookkeeping). cells.c writes what mode 0 draws from, which
core/vga/mode/mode0.h defines.

## What the console should be

- **A VT102**, minus what needs the hardware of a VT102: 132 columns, the
  keyboard LEDs (DECLL), the printer (MC), the answerback, interlace, and
  the alternate ROM character sets. Bugs are fixed as they are found.
- **What `xterm-256color` programs send**: curses programs, and other
  computers reached over telnet.
- **ANSI-BBS**: CP437, iCE colors, and SyncTERM's conventions.

## Memory

| | Cells | Beside the cells | Free |
|---|---|---|---|
| RP6502-PICO | 30.7 KB in VIDEO | 46.1 KB of colors, 12 KB of fonts, and 1 KB of palette in VIDEO | about 59 KB of lower RAM, before the stacks and heap |
| RP6502-FPGA | 57.6 KB in VIDCELLS | not built since the cells changed; see step 5 | |

A cell is 10 bytes in two tables. One has its glyph and 16 bits of
attributes, in one word, which is all a scanline reads. The other has its
three 16-bit colors (foreground, background, and underline), which are
read when a row's pixel pairs are built. A color is an RGB pixel or an
entry of mode 0's palette, so a palette change is one write to the
palette. The 40-column and the 80-column console both follow every byte,
and each has an alternate screen, so every table is paid four times.

The glyph is 16 bits: where the font has it. Each font has two tables of
256 glyphs, the code page's and one with the italics and DEC Special
Graphics, so a scanline draws any cell with one lookup and tests nothing.

The attributes: the two blinks, three bits of underline style, the
strike, the overline, whether the underline has a color of its own,
whether DECSCA or SPA guards the cell from the selective erases, whether
bold brightened the cell's ink (two bits), and whether its colors are
swapped. Four bits are free. Only the underlines, the strike, and the
overline change what a scanline draws, and only on their own rows of the
glyph: five of 16 rows. On the other rows a scanline does not read the
attributes. The guard, the bolds, and the swap are for DECCARA and
DECRARA, which change the attributes of cells already written.

The underlines are kitty's: SGR 4:0 to 4:5 for none, single, double,
curly, dotted, and dashed, with SGR 58 and 59 for the color. Each style's
pixels are in one table for each size of glyph, mode0_underline16 and
mode0_underline8, for mode0.c and mode0.sv both.

## The console's speed

The VGA holds 8 bytes from the RIA and cannot tell it to wait. At 115200 baud a
byte arrives every 17,500 cycles of core 0, a byte-time. One byte, with the
rest of the main loop's pass, must take under 8 byte-times, and a stream must
average under one.

Measured on a full 80x30 screen:

| | Cycles for the slowest byte | Byte-times | Sent back to back |
|---|---|---|---|
| REP 65535 | 27,700 | 1.6 | 22% |
| RIS | 12,100 | 0.7 | 36% |
| IL, DL, SU, and SD by 30 | 7,300 | 0.4 | 11% |
| SL and SR, any count | 5,200 | 0.3 | 8% |
| LF at the bottom | 1,600 | 0.1 | 9% |
| A character | 940 | 0.05 | 5% |
| A byte inside a sequence | 500 | 0.03 | 3% |

Everything else of a VT102 is under 0.4 byte-times.

Between left and right margins a scroll moves cells, and the rectangle
sequences change many cells at once:

| | Cycles for the slowest byte | Byte-times | Sent back to back |
|---|---|---|---|
| REP 65535 between margins | 73,500 | 4.2 | 52% |
| SL and SR by 1 between margins | 59,200 | 3.4 | 68% |
| DECSERA, 78 columns of every row | 58,700 | 3.4 | 26% |
| DECCRA, the screen a column right | 50,900 | 2.9 | 14% |
| DECFRA, 78 columns of every row | 43,700 | 2.5 | 16% |
| DECERA, 78 columns of every row | 36,100 | 2.1 | 16% |
| DECFRA, the whole screen | 20,900 | 1.2 | 20% |
| DECCARA or DECRARA, the whole screen | 19,800 | 1.1 | 5% |
| A character that wraps and scrolls between margins | 6,700 | 0.4 | 38% |
| LF, RI, IL, DL, SU, or SD between margins | 3,300 | 0.2 | 19% |

A row is owed one thing at a time: a fill, the cell every cell of it
becomes; a shift, the columns that survive, where they go, and the blank
color for each side; or a change of attributes, DECCARA's and DECRARA's.
An erase, DECALN, the whole rows of a REP, and the whole rows of DECFRA,
DECERA, and DECSERA are fills. SL and SR are shifts, and a second shift
of a row changes its record. What a row is owed is kept with the row's
memory, so a scroll moves none. The task loop pays one row a pass, and a
row is paid when it is written to. One 80-column row costs 610 to 700
cycles to move a cell sideways and 360 to fill. A move by an even count
of cells is faster, 244 cycles for 40 cells, because the colors stay on
a word.

A scroll between left and right margins cannot move a row of memory,
because the cells either side of the margins stay. The screen is owed
the scroll instead: the columns, the rows, how far, and the blank that
comes in. Another scroll of the same columns the same way adds to it, so
a run of line feeds moves the cells once, and anything else that changes
a cell does the scroll first. The task loop does it on its next pass,
4,600 to 9,900 cycles for a scroll by one row and 31,900 by 30. It
copies whichever is fewer: the cells between the margins up or down, or,
after moving the rows of memory, the cells either side back to their
rows. A REP between margins counts the scrolls under it first and does
them as one.

The row words are rewritten only when the layout they come from differs
from the one last written. On the Pico VGA the manifold sends each byte
to the USB port, a debug probe, and the console itself; the SDK's stdio
is one more source of bytes, for what the VGA prints.

A long REP's cycles are its rows, about 400 each to scroll and mark. The
40-column console wraps and scrolls twice as often as the 80-column one.
Working out the pen is 60 cycles of that, and keeping it between rows
measured no faster.

The HIL term test sends SL 1 and SR 1 200 times back to back, and REP
65535 twenty times, and loses no byte. Between margins it sends 200 line
feeds with no pause, 100 lines of one and of two characters, REP 65535
ten times, and SL 1 and SR 1 50 times, and then the rectangles over the
whole screen, and loses no byte.

On the Pico VGA, PIX's FIFO is read after every task of the main loop. It
goes unread for 5,800 cycles at most, the scanline core 0 renders, and
holds 3 of its 8 words at most, in a stream of REP 65535. An idle pass of
the loop is about 1,100 cycles.

## The target

Three planes with a fill layer on each, a program writing to the console
at 115200 baud, no late scanline, and a margin for sprites. It is met.

A mode 0 scanline averages 2,280 cycles of plain text, 2,250 with every
cell in its own colors, 2,700 with every cell underlined, and 3,350 with
every cell underlined in a curly line of its own color. A screen of those
over two more fill layers has no late scanline. With a fill layer on all
three planes a scanline averages 10,500 cycles and core 0's slowest is
13,900, 0.8 byte-times.

The margin is measured by making every scanline cost more cycles until
one is late. Over the console and two mode 2 layers at 8 bpp, on a VGA
display, it is 1,900 cycles a scanline with nothing written to the
console, 1,300 with a program printing plain text that scrolls, and 1,500
with a program printing text with every cell in its own colors.

The program prints at the 11,520 characters a second of 115200 baud. A
telnet session halves that for as long as it is connected, so the bench
reaches the console through a debug probe; see tests/hil/pico/README.md.

Every feature below is measured against the target, and kept or taken
out.

## 0. Smooth scroll, DECSCLM (?4), as an experiment

A VT100 scrolled smoothly at 6 lines a second, and told the computer
sending it text to wait (XOFF) when it fell behind. Nothing here can tell
a program to wait, so the text it writes during a scroll has to be held
somewhere, and at 115200 baud a full canvas of text arrives in about 0.2
s. The renderer would draw the scroll region at a pixel offset, which the
row table could carry. Build a demo ROM for the emulator and the
RP6502-PICO, look at it, then decide.

## 1. Answer what a program asks to find kitty's underlines

Decided: the underline styles and the underline's color stay, and a
program has to be able to find them. Neovim finds them by asking, not by
the console's name: with no `Smulx` in terminfo, which `xterm-256color`
has not, it sends `CSI 0 m`, `CSI 4:3 m`, and then DECRQSS for SGR,
`DCS $ q m ST`. An answer of `DCS 1 $ r 4:3 m ST`, or the same with `0;`
before the `4:3`, turns on every style and the underline's color, which
it sends as `CSI 58:2::r:g:b m`. With no answer every style is a plain
underline. The console drops every DCS string today, so it gives none.

- **DECRQSS for SGR.** Answer with the pen as it is: its attributes and
  its colors, the colors in the colon form. The same answer finds
  truecolor: with COLORTERM unset Neovim sets `CSI 48;2;1;2;3 m`, asks
  again, and wants `48:2:1:2:3` back. The answer goes out as the others
  do, so it has to fit the 48 bytes the answers wait in, or they grow.
- **The other settings DECRQSS can ask for** (DECSTBM, DECSLRM, DECSCUSR,
  DECSCA): decide with this step. xterm answers them.
- **A test that sends what Neovim sends when it starts**, byte for byte,
  and checks every answer and that the screen and the pen are left as
  they were.

What else Neovim sends when it starts, and what the console does with it
today, from reading both sources; not yet run:

- **DECRQM for ?69, ?2026, ?2027, ?2031, and ?2048.** Answered: ?69 as it
  is, the others as not recognized, which Neovim takes as "no".
- **`OSC 11 ; ?` and `CSI 5 n`**, for the background color. The DSR is
  answered, so Neovim does not warn. The OSC is not: a `?` is no color.
  Neovim then keeps its dark default, which is right for the console's
  default colors and wrong after a program changes them. Not decided:
  answer the `?` of OSC 4, 10, 11, and 12 as xterm does.
- **XTGETTCAP, `DCS + q ... ST`,** for truecolor and for the clipboard
  (`Ms`). Dropped with every other DCS string, which is a correct "no".
  There is no clipboard to offer.
- **`CSI ? u` and `CSI c`,** for kitty's keyboard protocol. Only the
  second is answered, so Neovim falls back to xterm's modifyOtherKeys,
  `CSI > 4 ; 2 m`, which the console ignores. Keys arrive as they do
  today.
- **`CSI > SP q`,** kitty's extra cursors. Ignored, and not mistaken for
  XTVERSION, which has no space.
- **`CSI 11 m`,** which Neovim uses as an alternate font. Ignored, now
  that SGR 11 is gone.
- **At exit, `CSI c` again.** Answered, so Neovim does not wait a second
  for it.

## 2. The manual

- Open the Terminal section of term.rst with what the console is: a
  VT102 minus 132 columns, the xterm subset, ANSI-BBS, and kitty's
  underlines. It does not list what a VT102's hardware did that this
  machine cannot.
- term.rst lists fast blink (SGR 6) and overline (SGR 53, 55) as features
  from other terminals. Both are ECMA-48's. The list loses them and gains
  xterm's, aixterm's, and SCO's.
- Remove `SGR 38:1` and `48:1`, which the console no longer has, and
  explain transparency by palette entry 0 and the default background.
- Remove SGR 10 and 11 and the Linux console's palette sequences
  (`ESC ] P` and `ESC ] R`), which the console no longer has. CRM
  (`CSI 3 h`) draws the controls.
- A palette change shows on text already drawn, and an underline's color
  follows the palette.
- XTVERSION: `CSI > q` is answered `DCS > | RP6502(version) ST`, the
  version being the firmware's as the status shows it. On the
  RP6502-PICO it is the RIA's.
- `CSI 11 t` answers that the window is minimized while the emulator's
  is. The RP6502-PICO, the RP6502-FPGA, and the web player always answer
  that it is not.
- DECLRMM (`?69`) and DECSLRM (`CSI Pl ; Pr s`), as xterm has them: what
  stops at the margins, that a scroll or an edit needs the cursor between
  them, that every line is single size while they are allowed, and that
  `CSI s` saves the cursor only while they are not. A program's end
  resets them.
- SL and SR do nothing with the cursor outside the scroll region, as on
  xterm.
- The rectangles: DECFRA, DECERA, DECSERA, DECCRA, DECCARA, DECRARA, and
  DECSACE. DECCARA and DECRARA take 1, 4, 5, and 7 (and 22, 24, 25, 27,
  and 0); bold is the bright color, as the pen draws it.
- Document each item above when it lands.

## 3. Use less memory

Do these together, after the steps above.

- The row color tables of modes 0 and 1 are 1.3 KB for each plane on
  each core, 8 KB in all, beside a 1 KB table for 4 bpp. A plane draws
  one or the other on a scanline, so the two can share their memory, and
  the 8 KB becomes about 1.6 KB. Not tried.
- Keep the cells of only one of the 40-column and 80-column consoles at a
  time and rebuild the other when the canvas changes. That halves the
  cells, if what the rebuild loses is acceptable.
- src/core/harvest.md lists memory to win back elsewhere.

## 4. scanvideo.c: decide at the end

What is found in scanvideo.c along the way is listed here. At the end,
decide whether to change it, rewrite it, or leave it.

- Taking a scanline buffer takes two locks and handing it back takes
  one. One lock for each would save an estimated 50 to 100 cycles a
  scanline. Not tried.

## 5. RP6502-FPGA: palette entries in cells

The cells now hold palette entries and the RP6502-FPGA still draws them
as pixels, so its console's colors are wrong until this is done. mode0.sv
needs a palette memory (one M10K block) written from mode0_palette, looks
up as many as three colors a cell in its 8 clocks, and needs a fit. The
cells are two memories now, the glyphs and attributes in one and the
colors in the other, the fonts have a second table each, and the
underlines have their styles. A pending wrap is a bit of the cursor word
(MODE0_CURSOR_PENDING), not a column past the last, and the attributes
have three bits no renderer draws.

## Not doing

- **Kitty's graphics protocol.** The terminal keeps the images a program
  sends, often megabytes. There is no room, and a program draws pictures
  on the planes.
- **Kitty's keyboard protocol.** A program asks for it on its own, with
  `CSI ? u`, and finding the underlines does not turn it on. Without it
  Neovim, Helix, and fish read keys as they do on xterm. It would have
  roughly doubled the key encoder (vtkey.c) and needed key releases,
  which have no path from the keyboard today.
- **Kitty's other protocols**: notifications, color control by name (OSC
  21, which overlaps OSC 4, 10, and 11), and text in larger sizes. Few
  programs use them.
- **Synchronized output (?2026).** There is only one copy of the cells,
  so text written over text already showing cannot be held back. Holding
  back only the row table and the cursor would mislead a program that
  asks for the whole.
- **The hardware of a VT102**: 132 columns, interlace, the keyboard LEDs,
  the printer, and the answerback.
