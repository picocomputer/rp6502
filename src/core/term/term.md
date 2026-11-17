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
| RP6502-PICO | 30.7 KB in VIDEO | 46.1 KB of colors and 1 KB of palette in VIDEO | about 62 KB of lower RAM, before the stacks and heap |
| RP6502-FPGA | 57.6 KB in VIDCELLS | not built since the cells changed; see step 13 | |

A cell is 10 bytes in two tables. One has its glyph, an unused byte, and
16 bits of attributes, in one word, which is all a scanline reads. The
other has its three 16-bit colors (foreground, background, and
underline), which are read when a row's pixel pairs are built. A color is
an RGB pixel or an entry of mode 0's palette, so a palette change is one
write to the palette. The low byte of the attributes is what the renderer
draws; one bit above it says DECSCA or SPA guards the cell from the
selective erases, and seven are free. The 40-column and the 80-column
console both follow every byte, and each has an alternate screen, so
every table is paid four times.

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

A mode 0 scanline is 2,530 cycles of plain text and 2,330 with every cell
in its own colors. With a fill layer on all three planes a scanline
averages 10,500 cycles and core 0's slowest is 13,900, 0.8 byte-times.

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

## 0. Kitty's underlines: all or nothing

**What they are.** Underline styles chosen with a colon parameter: 4:0
none, 4:1 single, 4:2 double, 4:3 curly, 4:4 dotted, and 4:5 dashed. SGR
58 sets the underline's color and SGR 59 returns it to the foreground's.
Editors mark errors and warnings with a colored curly underline. Most
modern terminals have them, and terminfo names them (Smulx, Setulc).

**Today.** The console accepts all of it but draws only single and
double. Curly, dotted, and dashed draw as single.

**The choice:**

- **All.** Draw every style, with SGR 58 and 59. Curly is a wave 3 pixels
  tall on 8x16 cells and 2 on 8x8. One table of each style's pixels
  serves mode0.c and mode0.sv. The six styles need three bits and the
  attribute byte has two for them, so the style goes in the byte of
  extras beside each cell, and a bit in the cell's attributes says the
  cell has an underline. Bits may move between the cell and its extras,
  and bits that only tell the renderer what to draw may be added,
  wherever that draws fastest.
- **None.** SGR 4:n, 58, and 59 go, and the underline color leaves the
  cell. A cell shrinks from 8 bytes to 6: about 15 KB saved on the
  RP6502-PICO and 14 KB on the RP6502-FPGA. The cell's layout changes in
  mode0.h, mode0.c, and mode0.sv, and the RP6502-FPGA needs a fit.

All, to begin with; it can come out later if it is not wanted.

## 1. Kitty's keyboard protocol, only if 0 is "all"

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

## 2. Drop the Linux console's palette sequences

`ESC ] P nrrggbb` and `ESC ] R` set and reset palette colors on the Linux
console. xterm's OSC 4 and OSC 104 do the same and more: any of the 256
entries, several in one sequence, X11 color names, and resetting one
entry or all. Remove the Linux ones; `ESC ] P` then starts an ordinary
OSC that runs to its BEL or ST, as on xterm.

## 3. Bracketed paste (mode ?2004)

When a program turns it on, pasted text arrives wrapped in `ESC [ 200 ~`
and `ESC [ 201 ~`. An editor can then tell a paste from typing and not
indent it, and a line editor does not run a pasted line. Any ESC inside
the pasted text is dropped so the text cannot end the wrapper early.
DECRQM reports the mode and RIS clears it. Paste comes from the emulator
and the web player.

## 4. Report the console's name (XTVERSION)

A program sends `CSI > q` and the console answers
`DCS > | RP6502(0.25) ST`, so the program need not guess the terminal
from its device attributes. The version is the firmware's, without a
"v", or the CI build number.

## 5. Rectangle sequences (VT420)

DECFRA (fill), DECERA (erase), DECSERA (selective erase), DECCRA (copy),
DECCARA and DECRARA (change and reverse attributes), and DECSACE, which
chooses stream or rectangle. xterm has them, and DECSERA completes the
protected areas.

## 6. Left and right margins (DECSLRM)

Scrolling and editing limited to a column range, allowed by mode ?69
(DECLRMM). tmux and some editors use it when it is there. Every insert,
delete, and scroll has to respect the margins.

## 7. Window size reports (`CSI t`)

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

## 8. Drop SGR 10 and 11

On the Linux console, SGR 11 makes bytes 1 to 31 and DEL print as their
CP437 pictures and SGR 10 turns that off. The standard way is CRM
(`CSI 3 h` and `CSI 3 l`), which the console already has and which sets
the same switch. Remove SGR 10 and 11. The target is xterm, not the Linux
console.

## 9. Smooth scroll, DECSCLM (?4), as an experiment

A VT100 scrolled smoothly at 6 lines a second, and told the computer
sending it text to wait (XOFF) when it fell behind. Nothing here can tell
a program to wait, so the text it writes during a scroll has to be held
somewhere, and at 115200 baud a full canvas of text arrives in about 0.2
s. The renderer would draw the scroll region at a pixel offset, which the
row table could carry. Build a demo ROM for the emulator and the
RP6502-PICO, look at it, then decide.

## 10. The manual

- Open the Terminal section of term.rst with what the console is: a
  VT102 minus 132 columns, the xterm subset, ANSI-BBS, and kitty's
  underlines and keyboard protocol if 4 and 5 land. It does not list what
  a VT102's hardware did that this machine cannot.
- term.rst lists fast blink (SGR 6) and overline (SGR 53, 55) as features
  from other terminals. Both are ECMA-48's. The list loses them and gains
  xterm's, aixterm's, and SCO's.
- Remove `SGR 38:1` and `48:1`, which the console no longer has, and
  explain transparency by palette entry 0 and the default background.
- A palette change shows on text already drawn, and an underline's color
  follows the palette.
- Document each item above when it lands.

## 11. Use less memory

Do these together, after the steps above.

- The row color tables of modes 0 and 1 are 1.3 KB for each plane on
  each core, 8 KB in all, beside a 1 KB table for 4 bpp. A plane draws
  one or the other on a scanline, so the two can share their memory, and
  the 8 KB becomes about 1.6 KB. Not tried.
- Dropping kitty's underlines (4) removes a quarter of each cell.
- Keep the cells of only one of the 40-column and 80-column consoles at a
  time and rebuild the other when the canvas changes. That halves the
  cells, if what the rebuild loses is acceptable.
- src/core/harvest.md lists memory to win back elsewhere.

## 12. scanvideo.c: decide at the end

What is found in scanvideo.c along the way is listed here. At the end,
decide whether to change it, rewrite it, or leave it.

- Taking a scanline buffer takes two locks and handing it back takes
  one. One lock for each would save an estimated 50 to 100 cycles a
  scanline. Not tried.

## 13. RP6502-FPGA: palette entries in cells

The cells now hold palette entries and the RP6502-FPGA still draws them
as pixels, so its console's colors are wrong until this is done. mode0.sv
needs a palette memory (one M10K block) written from mode0_palette, looks
up as many as three colors a cell in its 8 clocks, and needs a fit. Once
a renderer reads the extras beside the cells, as for step 0, it needs a
memory for those too.

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
