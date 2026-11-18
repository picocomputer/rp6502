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
| RP6502-FPGA | 28.8 KB in block memory | 43.2 KB of colors, 12 KB of fonts, and 1 KB of palette in block memory | about 47 KB of the firmware's 64 KB, before the stack and heap |

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
mode0_underline8. mode0.sv has the same pixels, and a test draws every
style on both renderers and compares the frames.

A program finds the underlines by asking. Neovim sets `CSI 4:3 m` and
sends DECRQSS for SGR, `DCS $ q m ST`; the console answers with the pen,
`DCS 1 $ r 0;4:3 m ST`, and Neovim then uses every style and the
underline's color. It finds truecolor the same way, so the pen keeps an
RGB color's three channels as they were given. Kitty's keyboard protocol
is asked for apart from this, with `CSI ? u`, and is not answered.

The answers wait in 256 bytes on the machine that gives them, which on
the RP6502-PICO is the RIA. One that does not fit is dropped whole. All
that Neovim asks as it starts is 152 bytes of answers. The RIA follows
the palette, 518 bytes, to answer what a color is.

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

There are ten scanline buffers, 3,908 bytes each. Over the terminal
stream on the worst canvas, 30 seconds on each display, five and six
buffers showed a missing line and seven showed none. Ten stays.

The line interrupt starts the scanline DMA 160 cycles after it is
entered, 179 at most, at 1280x1024, where it has 720. Core 0 takes about
1.5 times its own cycles in wall time, and the line interrupt does not
explain it. Not found yet.

## The RP6502-FPGA

mode0.sv draws the console from the same cells, colors, palette, and
fonts, which the linker places on block memories, so cells.c and font.c
write them where they are drawn from. The colors are in memory as the
firmware has them, three halfwords a cell, and the scanout reads the two
words a cell's colors are in.

There is one console, so it has an engine of its own beside the fill
engine, and a line to finish in. A cell is 8 clocks. Its word, its
colors, a row of its glyph, and a palette entry for each of its three
colors are fetched in the 8 clocks of the cell before it, one read a
clock. An 80-column line takes 655 clocks of 1,600 and a 40-column row
335 of 3,200.

Modes 1, 2, and 3 can be on every plane, and share the fill engine, so
each must finish a 640-pixel line in a third of it, 532 clocks. The
slowest measured takes 478, and three of them finish at 1,424 of the
1,598 a line has. The console over two 8 bpp text planes, all three
reading the one font store, finishes at 979.

It fits in 305 of the 308 memory blocks, with 0.8 ns of setup slack: 32
blocks of cells, 48 of colors, 11 of fonts, and 4 of palette.

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
- **Smooth scroll, DECSCLM (?4).** Tried on the RP6502-PICO: the picture
  lagged the terminal by up to four spare rows of cells and eased up to
  it, and jumped when it could not keep up. It could not keep up with
  much. At 115200 baud 192 bytes arrive in a frame, so full 80-column
  lines scroll 2.3 rows a frame, and one row a frame would take lines of
  190 characters. A VT100 held the computer off with XOFF, and nothing
  here can.
- **Synchronized output (?2026).** There is only one copy of the cells,
  so text written over text already showing cannot be held back. Holding
  back only the row table and the cursor would mislead a program that
  asks for the whole.
- **The hardware of a VT102**: 132 columns, interlace, the keyboard LEDs,
  the printer, and the answerback.
