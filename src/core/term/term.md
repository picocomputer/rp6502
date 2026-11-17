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
| RP6502-PICO | 61.4 KB in VIDEO | 23 KB in lower RAM | about 66 KB of lower RAM, before the stacks and heap |
| RP6502-FPGA | 57.6 KB in VIDCELLS | 21.6 KB of the 64 KB RAM block | about 21 KB of the RAM block; 192 KB more in its window for the block to grow |

A cell is 8 bytes: glyph, attributes, and three 16-bit colors
(foreground, background, and underline). The color sources hold the
palette entry or default color each color came from, so a palette change
can recolor the cells. The 40-column and the 80-column console both
follow every byte, and each has an alternate screen, so every table is
paid four times.

## 0. Mode 0: prepare a row's colors once

Steps 0 to 3 make the fill layers draw faster on the RP6502-PICO. The
measure is the cycles the VGA takes to draw one scanline. Its two cores
have about 12,800 between them at 640x480.

Mode 0, the console, takes 3,286 cycles a scanline. For every cell on
every scanline, it reads the cell's colors and builds a small table from
them. A row is 16 scanlines in the same colors, so build the tables on
the row's first scanline and reuse them.

Tried: about 2,360 cycles a scanline, for 2.6 KB of RAM. Not reviewed.

## 1. Mode 1: prepare a row's colors once

Mode 1 at 16-bit color with 8x16 cells takes 2,668 cycles a scanline. It
draws cells the way mode 0 does, so try what step 0 does. If the RAM for
that is too much, build a cell's table only when its colors differ from
the cell before it. Not tried.

## 2. Mode 2 at 8 bpp, and the other fill modes

Mode 2 at 8 bpp with 8x8 tiles is the costliest fill layer measured:
3,452 cycles a scanline, and two such layers about 7,360. Its loop is
about 4.3 instructions a pixel, and an instruction is about a cycle.
Nothing to gain has been found yet. The DSP instructions are not tried.

Profile the other fill modes, costliest first.

## 3. The work outside the fill layers

Each scanline costs 300 to 390 cycles outside the fill layers. Find where
they go and cut what is not needed. Not tried.

## 4. Console: speed up character handling

The console has grown since it was last measured on hardware, and it
takes core 0 away from drawing while a program writes to it. It can be
rearchitected for speed. Measured:

- Every byte costs about 1,900 cycles. After each one the console
  rewrites the row words of all 30 rows, resolves the pen's colors once
  for each of the two console sizes, and the byte comes in through the
  SDK's stdio. Tried: about 800 cycles. Not reviewed, and it needs a
  test.
- REP, SL, and SR do all their work in one byte: REP with a count of
  65535 takes as long as drawing 25 scanlines, SL and SR by 40 columns as
  long as 11. Measure every sequence that works on every row at once.
  Not designed.
- The main loop spends about 1,100 cycles a pass on tasks with nothing to
  do. Tried: about 340. Not reviewed.

## 5. The target

Three planes with a fill layer on each, a program writing to the console
at 115200 baud, no late scanline, and a margin for sprites. The HIL term
test over the console and two mode 2 layers at 8 bpp loses scanlines
today.

Every feature below is then measured against the target, and kept or
taken out.

The cycle counters that take these measurements are on the local branch
spike-palette.

## 6. Spike: palette entries in cells (done)

A cell's 16-bit color is told apart by the opaque bit of the pixel
format, bit 5:

- **Opaque**: a pixel, drawn as it is. SGR 38;2 colors are stored this
  way, with no lookup.
- **Not opaque**: the other 15 bits hold an entry in a color table (the
  256 palette entries, the default foreground, and the default
  background) and a bit for faint. One shift takes the entry out.
  Transparency comes from the table.

Bold and iCE colors still choose their entry as the text is written, and
SGR 7 still swaps the two colors at the cell.

Measured with the console over two mode 2 layers at 8 bpp, every cell a
different color, cycles a scanline:

| | avg | max | late lines |
|---|---|---|---|
| Colors stored as pixels, as now | 10,553 | 10,911 | 0 |
| Looked up for every cell, every scanline | 12,221 | 13,509 | 21,952 in 2 s |
| Looked up once a row, then reused | 11,072 | 12,868 | 0 |

Looked up once a row, it costs about 5% there, about 1,500 cycles on the
first scanline of each row, and nothing on ordinary text.

Looking every cell up once a frame was rejected: a row that scrolls in
would show the colors of the row it replaced for a frame.

## 7. Store palette entries in cells

Do this after 0 to 3, if the fill rate has room for the lookup.

- **Saves about 21 KB** on the RP6502-PICO and 21.6 KB on the
  RP6502-FPGA: the color sources, and the pass that recolors cells after
  a palette change, both go. VIDEO gains about 1 KB for the color table
  and 0.65 KB a core for a row's colors.
- **Palette changes show on the next frame**, as on xterm.
- **The underline color follows the palette for free.**
- **The color table joins mode0.h**, and cells.c writes it.
- **The RP6502-FPGA needs a palette memory** (one M10K block), looks up
  as many as three colors a cell in its 8 clocks, and needs a fit.

If the fill rate has no room, cells stay as they are. An underline color
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

## Ways to use less memory

To work out with the plan, not after it.

- Palette entries in cells (7) remove the color sources.
- Dropping kitty's underlines (8) removes a quarter of each cell.
- Keep the cells of only one of the 40-column and 80-column consoles at a
  time and rebuild the other when the canvas changes. That halves the
  cells, if what the rebuild loses is acceptable.
- src/core/harvest.md lists memory to win back elsewhere.
