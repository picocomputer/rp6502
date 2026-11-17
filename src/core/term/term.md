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

| | Cells | Color sources | Free |
|---|---|---|---|
| RP6502-PICO | 61.4 KB in VIDEO | 23 KB in lower RAM | about 54 KB of lower RAM, before the stacks and heap |
| RP6502-FPGA | 57.6 KB in VIDCELLS | 21.6 KB of the 64 KB RAM block | about 21 KB of the RAM block; 192 KB more in its window for the block to grow |

A cell is 8 bytes: glyph, attributes, and three 16-bit colors
(foreground, background, and underline). The color sources hold the
palette entry or default color each color came from, so a palette change
can recolor the cells. The 40-column and the 80-column console both
follow every byte, and each has an alternate screen, so every table is
paid four times.

## 0. Runs of cells

Steps 0 to 5 speed up the console. The VGA holds 8 bytes from the RIA and
cannot tell it to wait. At 115200 baud a byte arrives every 17,500 cycles
of core 0, a byte-time. One byte, with the rest of the main loop's pass,
must take under 8 byte-times, and a stream must average under one.

Measured on a full 80x30 screen:

| | Cycles for the slowest byte | Byte-times | Sent back to back |
|---|---|---|---|
| REP 65535 | 133,000 | 7.6 | 101% |
| SR 1 | 132,000 | 7.6 | 157% |
| SL 1 | 124,000 | 7.1 | 147% |
| REP 2400 | 114,000 | 6.5 | 99% |
| SL and SR by 40 or 80 | 65,000 to 82,000 | 3.7 to 4.7 | 68 to 84% |
| LF at the bottom | 9,700 | 0.55 | 55% |
| A character | 1,750 | 0.1 | 10% |

Everything else is under 0.6 byte-times. SL 1 and SR 1 sent back to back
lose bytes.

One 80-column row costs 2,500 cycles to move a cell sideways, 1,400 to
blank, and 2,000 to fill with a glyph. Of the move, 1,790 is the color
sources: 3-byte entries, which the C library copies a byte at a time.

Write two routines for the cells and their color sources: one writes a
run of cells with one value, and one moves a run. A cell is two words.
Measure each against the numbers above before anything uses it.

## 1. REP

REP already stops after one screen and a row, so 65535 costs what 2400
does. 84,000 of REP 2400's cycles are the loop that writes the cells, at
25 cycles a cell. Use the routine of step 0.

If REP is still over 2 byte-times, mark whole rows as owed the glyph, as
an erase marks them.

## 2. LF

5,000 of a line feed's cycles move the erase each row is owed up one
place, 29 rows on each console size. 2,100 blank the new bottom row.

Keep each owed erase with its row's memory, as the row map is kept, so a
scroll of the screen moves none. Blank the new row with the routine of
step 0.

## 3. SL and SR

A row records the shift it is owed: the columns that survive, where they
go, and the blank color for each side. The command marks the rows and
moves nothing. The task loop settles one row a pass, and a row is settled
when it is written to. A shift of a row that already owes one changes the
record. A row is settled early only when one side is given a second
color.

The HIL term test sends SL 1 and SR 1 200 times back to back, and no
byte is lost.

The savestate's console chunk changes with the record.

## 4. Each byte

After every byte the console rewrites the row words of all 30 rows, 720
cycles of a character's 1,750. It resolves the pen's colors once for each
of the two console sizes, and the byte comes in through the SDK's stdio.

Tried: about 800 cycles a byte. Not reviewed, and it needs a test.

## 5. The main loop

The main loop spends about 1,100 cycles a pass on tasks with nothing to
do. Tried: about 340. Not reviewed.

## 6. The target

Three planes with a fill layer on each, a program writing to the console
at 115200 baud, no late scanline, and a margin for sprites.

The margin is measured by making every scanline cost more cycles until
one is late. Over the console and two mode 2 layers at 8 bpp, on a VGA
display, it is 2,000 cycles a scanline with nothing written to the
console, 1,350 with a program printing plain text that scrolls, and 1,600
with a program printing text with every cell in its own colors.

The program prints at the 11,520 characters a second of 115200 baud. A
telnet session halves that for as long as it is connected, so the bench
reaches the console through a debug probe; see tests/hil/pico/README.md.

Every feature below is measured against the target, and kept or taken
out.

## 7. Store palette entries in cells

Not done. A spike on the local branch spike-palette measured it; the
cells on this branch still hold 16-bit colors and their color sources.

A cell's 16-bit color would be told apart by the opaque bit of the pixel
format, bit 5:

- **Opaque**: a pixel, drawn as it is. SGR 38;2 colors are stored this
  way, with no lookup.
- **Not opaque**: the other 15 bits hold an entry in a color table (the
  256 palette entries, the default foreground, and the default
  background) and a bit for faint. One shift takes the entry out.
  Transparency comes from the table.

Bold and iCE colors would still choose their entry as the text is
written, and SGR 7 would still swap the two colors at the cell.

Mode 0 would look the entries up once a row, where it builds the row's
color tables. In the spike that cost about 5% over the console and two
mode 2 layers at 8 bpp with every cell a different color, and nothing on
ordinary text. Do this if the margin of step 6 has room for it.

- **Saves about 21 KB** on the RP6502-PICO and 21.6 KB on the
  RP6502-FPGA: the color sources, and the pass that recolors cells after
  a palette change, both go. VIDEO gains about 1 KB for the color table.
- **A row of cells moves in 700 cycles, not 2,500**, with no color
  sources to move.
- **Palette changes show on the next frame**, as on xterm.
- **The underline color follows the palette for free.**
- **The color table joins mode0.h**, and cells.c writes it.
- **The RP6502-FPGA needs a palette memory** (one M10K block), looks up
  as many as three colors a cell in its 8 clocks, and needs a fit.

If the margin has no room, cells stay as they are. An underline color
from the palette then needs a third source for each cell to follow
palette changes: about 7.5 KB on the RP6502-PICO and 7 KB on the
RP6502-FPGA.

## 8. Kitty's underlines: all or nothing

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
  serves mode0.c and mode0.sv. The style needs one more bit: the
  underline and double underline bits give four states, one unused, and
  the attribute byte is full. A color that holds a palette entry has spare
  bits and a color that holds a pixel does not, so where the bit goes
  depends on 7.
- **None.** SGR 4:n, 58, and 59 go, and the underline color leaves the
  cell. A cell shrinks from 8 bytes to 6: about 15 KB saved on the
  RP6502-PICO and 14 KB on the RP6502-FPGA. The cell's layout changes in
  mode0.h, mode0.c, and mode0.sv, and the RP6502-FPGA needs a fit.

Decided after 7.

## 9. Kitty's keyboard protocol, only if 8 is "all"

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

## 10. Drop the Linux console's palette sequences

`ESC ] P nrrggbb` and `ESC ] R` set and reset palette colors on the Linux
console. xterm's OSC 4 and OSC 104 do the same and more: any of the 256
entries, several in one sequence, X11 color names, and resetting one
entry or all. Remove the Linux ones; `ESC ] P` then starts an ordinary
OSC that runs to its BEL or ST, as on xterm.

## 11. Bracketed paste (mode ?2004)

When a program turns it on, pasted text arrives wrapped in `ESC [ 200 ~`
and `ESC [ 201 ~`. An editor can then tell a paste from typing and not
indent it, and a line editor does not run a pasted line. Any ESC inside
the pasted text is dropped so the text cannot end the wrapper early.
DECRQM reports the mode and RIS clears it. Paste comes from the emulator
and the web player.

## 12. Report the console's name (XTVERSION)

A program sends `CSI > q` and the console answers
`DCS > | RP6502(0.25) ST`, so the program need not guess the terminal
from its device attributes. The version is the firmware's, without a
"v", or the CI build number.

## 13. Rectangle sequences (VT420)

DECFRA (fill), DECERA (erase), DECSERA (selective erase), DECCRA (copy),
DECCARA and DECRARA (change and reverse attributes), and DECSACE, which
chooses stream or rectangle. xterm has them, and DECSERA completes the
protected areas.

## 14. Left and right margins (DECSLRM)

Scrolling and editing limited to a column range, allowed by mode ?69
(DECLRMM). tmux and some editors use it when it is there. Every insert,
delete, and scroll has to respect the margins.

## 15. Drop the special transparent color

`SGR 38:1` and `48:1` are the console's own way to ask for a transparent
color. No other terminal draws them, and mintty, foot, and iTerm2 read
the `38;1` form as bold. They are not needed: palette entry 0 and the
default background are transparent until a program changes them, so text
over a picture on a plane behind the console shows the picture there. A
program that sets entry 0 with OSC 4 gets an opaque color, which is what
it expects. Remove them; `38:1` and `48:1` become forms SGR does not
know, skipped whole like any other.

## 16. Window size reports (`CSI t`)

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

## 17. Drop SGR 10 and 11

On the Linux console, SGR 11 makes bytes 1 to 31 and DEL print as their
CP437 pictures and SGR 10 turns that off. The standard way is CRM
(`CSI 3 h` and `CSI 3 l`), which the console already has and which sets
the same switch. Remove SGR 10 and 11. The target is xterm, not the Linux
console.

## 18. Smooth scroll, DECSCLM (?4), as an experiment

A VT100 scrolled smoothly at 6 lines a second, and told the computer
sending it text to wait (XOFF) when it fell behind. Nothing here can tell
a program to wait, so the text it writes during a scroll has to be held
somewhere, and at 115200 baud a full canvas of text arrives in about 0.2
s. The renderer would draw the scroll region at a pixel offset, which the
row table could carry. Build a demo ROM for the emulator and the
RP6502-PICO, look at it, then decide.

## 19. The manual

- Open the Terminal section of term.rst with what the console is: a
  VT102 minus 132 columns, the xterm subset, ANSI-BBS, and kitty's
  underlines and keyboard protocol if 8 and 9 land. It does not list what
  a VT102's hardware did that this machine cannot.
- term.rst lists fast blink (SGR 6) and overline (SGR 53, 55) as features
  from other terminals. Both are ECMA-48's. The list loses them and gains
  xterm's, aixterm's, and SCO's.
- With 15, remove `SGR 38:1` and `48:1` and explain transparency by
  palette entry 0 and the default background.
- Document each item above when it lands.

## 20. Use less memory

Do these together, after the steps above.

- The row color tables of modes 0 and 1 are 1.3 KB for each plane on
  each core, 8 KB in all, beside a 1 KB table for 4 bpp. A plane draws
  one or the other on a scanline, so the two can share their memory, and
  the 8 KB becomes about 1.6 KB. Not tried.
- Palette entries in cells (7) remove the color sources.
- Dropping kitty's underlines (8) removes a quarter of each cell.
- Keep the cells of only one of the 40-column and 80-column consoles at a
  time and rebuild the other when the canvas changes. That halves the
  cells, if what the rebuild loses is acceptable.
- src/core/harvest.md lists memory to win back elsewhere.

## 21. scanvideo.c: decide at the end

What is found in scanvideo.c along the way is listed here. At the end,
decide whether to change it, rewrite it, or leave it.

- Taking a scanline buffer takes two locks and handing it back takes
  one. One lock for each would save an estimated 50 to 100 cycles a
  scanline. Not tried.

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
