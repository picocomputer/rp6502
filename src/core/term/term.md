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
| RP6502-FPGA | 57.6 KB in VIDCELLS | not built since the cells changed; see step 8 | |

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
strike, the overline, whether the underline has a color of its own, and
whether DECSCA or SPA guards the cell from the selective erases. Seven
bits are free. Only the underlines, the strike, and the overline change what a
scanline draws, and only on their own rows of the glyph: five of 16 rows.
On the other rows a scanline does not read the attributes.

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

Everything else is under 0.4 byte-times.

A row is owed one thing at a time: a fill, the cell every cell of it
becomes, or a shift, the columns that survive, where they go, and the
blank color for each side. An erase, DECALN, and the whole rows of a REP
are fills. SL and SR are shifts, and a second shift of a row changes its
record. What a row is owed is kept with the row's memory, so a scroll
moves none. The task loop pays one row a pass, and a row is paid when it
is written to. One 80-column row costs 610 to 700 cycles to move a cell
sideways and 360 to fill. A move by an even count of cells is faster, 244
cycles for 40 cells, because the colors stay on a word.

The row words are rewritten only when the layout they come from differs
from the one last written. On the Pico VGA the manifold sends each byte
to the USB port, a debug probe, and the console itself; the SDK's stdio
is one more source of bytes, for what the VGA prints.

A long REP's cycles are its rows, about 400 each to scroll and mark. The
40-column console wraps and scrolls twice as often as the 80-column one.
Working out the pen is 60 cycles of that, and keeping it between rows
measured no faster.

The HIL term test sends SL 1 and SR 1 200 times back to back, and REP
65535 twenty times, and loses no byte.

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

## 0. Rectangle sequences (VT420)

DECFRA (fill), DECERA (erase), DECSERA (selective erase), DECCRA (copy),
DECCARA and DECRARA (change and reverse attributes), and DECSACE, which
chooses stream or rectangle. xterm has them, and DECSERA completes the
protected areas.

## 1. Left and right margins (DECSLRM)

Scrolling and editing limited to a column range, allowed by mode ?69
(DECLRMM). tmux and some editors use it when it is there. Every insert,
delete, and scroll has to respect the margins.

## 2. Window size reports (`CSI t`)

Keep the reports; ignore the rest of `CSI t`, which moves and resizes
desktop windows.

- **11, is the window minimized?** Always "no" on the RP6502-PICO and the
  RP6502-FPGA. The emulator can answer "yes" while its window is
  minimized, which Sokol reports, and the web player while its page is
  hidden. Where that is awkward, the answer is "no".
- **14 and 16, sizes in pixels** of the text area and of one character.
  A program drawing on the planes behind the console can line up with
  the text.
- **18 and 19, sizes in characters.** Both give the size the console is
  shown at, or the size a program set with RIA_ATTR_RLN_WIDTH and HEIGHT.
  They always match.

## 3. Smooth scroll, DECSCLM (?4), as an experiment

A VT100 scrolled smoothly at 6 lines a second, and told the computer
sending it text to wait (XOFF) when it fell behind. Nothing here can tell
a program to wait, so the text it writes during a scroll has to be held
somewhere, and at 115200 baud a full canvas of text arrives in about 0.2
s. The renderer would draw the scroll region at a pixel offset, which the
row table could carry. Build a demo ROM for the emulator and the
RP6502-PICO, look at it, then decide.

## 4. Kitty's keyboard protocol

**What it is.** A program reading stdin can tell every key apart without
the RIA's keyboard registers. Neovim, Helix, and fish use it.

- **The flags.** A program pushes, pops, sets, and queries them with
  `CSI > n u`, `CSI < u`, `CSI = n u`, and `CSI ? u`. Each screen keeps a
  small stack. They go in term.c, which the RIA's terminal shares.
- **The keys.** Each is reported as `CSI code ; modifiers u`. The code is
  the Unicode code point of the key's unshifted character, or one from
  kitty's table for keys with no character. The modifiers include Super,
  Hyper, Meta, Caps Lock, and Num Lock.
- **What the flags ask for.** Telling apart keys that send the same bytes
  today (Tab and Ctrl+I, Enter and Ctrl+M, Escape and Alt), repeats and
  releases, alternate keys, every key as a sequence, and the text a key
  types. Programs often ask for the first alone.

**The cost.** The key encoder (vtkey.c, about 220 lines) roughly doubles.
Releases need a path from the keyboard that does not exist today. A few
bytes of flags and a table of about a hundred key codes. About a week
with tests.

## 5. The manual

- Open the Terminal section of term.rst with what the console is: a
  VT102 minus 132 columns, the xterm subset, ANSI-BBS, kitty's
  underlines, and kitty's keyboard protocol if step 4 lands. It does not
  list what a VT102's hardware did that this machine cannot.
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
- Document each item above when it lands.

## 6. Use less memory

Do these together, after the steps above.

- The row color tables of modes 0 and 1 are 1.3 KB for each plane on
  each core, 8 KB in all, beside a 1 KB table for 4 bpp. A plane draws
  one or the other on a scanline, so the two can share their memory, and
  the 8 KB becomes about 1.6 KB. Not tried.
- Dropping the underline's color removes a fifth of each cell, and SGR
  58 and 59 with it.
- Keep the cells of only one of the 40-column and 80-column consoles at a
  time and rebuild the other when the canvas changes. That halves the
  cells, if what the rebuild loses is acceptable.
- src/core/harvest.md lists memory to win back elsewhere.

## 7. scanvideo.c: decide at the end

What is found in scanvideo.c along the way is listed here. At the end,
decide whether to change it, rewrite it, or leave it.

- Taking a scanline buffer takes two locks and handing it back takes
  one. One lock for each would save an estimated 50 to 100 cycles a
  scanline. Not tried.

## 8. RP6502-FPGA: palette entries in cells

The cells now hold palette entries and the RP6502-FPGA still draws them
as pixels, so its console's colors are wrong until this is done. mode0.sv
needs a palette memory (one M10K block) written from mode0_palette, looks
up as many as three colors a cell in its 8 clocks, and needs a fit. The
cells are two memories now, the glyphs and attributes in one and the
colors in the other, the fonts have a second table each, and the
underlines have their styles.

## Not doing

- **Kitty's graphics protocol.** The terminal keeps the images a program
  sends, often megabytes. There is no room, and a program draws pictures
  on the planes.
- **Kitty's other protocols**: notifications, color control by name (OSC
  21, which overlaps OSC 4, 10, and 11), and text in larger sizes. Few
  programs use them.
- **Synchronized output (?2026).** There is only one copy of the cells,
  so text written over text already showing cannot be held back. Holding
  back only the row table and the cursor would mislead a program that
  asks for the whole.
- **The hardware of a VT102**: 132 columns, interlace, the keyboard LEDs,
  the printer, and the answerback.
