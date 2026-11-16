# The console terminal: the plan

The terminal is three files: term.c (the parser and everything the same at
every size), grid.c (the terminal at one size: cursor, margins, tabs, line
sizes) and cells.c (the screen's cells and their bookkeeping). cells.c
writes what mode 0 draws from, which core/vga/mode/mode0.h defines.

## The target

Three layers, and anything outside them has to earn its place:

1. **The VT102**, apart from what needs a real terminal's hardware: 132
   columns, the keyboard LEDs (DECLL), the printer (MC), the answerback,
   interlace and the alternate ROM character sets. It is compliant enough;
   bugs are fixed as they are found.
2. **The xterm subset that `xterm-256color` terminfo uses**, which is what
   ported curses code and remote hosts send.
3. **ANSI-BBS**: CP437, iCE colors and SyncTERM's conventions.

## Memory now

| | Cells (mode0_cells) | Color sources (cells_srcs) | Free |
|---|---|---|---|
| Pico VGA | 61.4 KB in VIDEO | 23 KB in lower RAM | about 66 KB of lower RAM, before the stacks and heap |
| Pocket | 57.6 KB in VIDCELLS | 21.6 KB of the 64 KB RAM block | about 21 KB of the RAM block, which no heap uses; 192 KB more in its window for the block to grow |

A cell is 8 bytes: its glyph, its attributes, and its foreground,
background and underline colors as resolved RGB, 16 bits each. The color
sources say which palette entry or default color each cell's colors came
from, so a change to one can recolor the cells drawn in it, a row a tick
in cells_task. Both sizes, 40 and 80 columns, follow every byte, and both
have an alternate screen, so every per-cell table is paid four times over.

## 0. The fill rate first

Making the machine fast is this branch's first goal, before any feature
here is added: each feature is then measured against the budget and kept
or taken out.

The spike in 1 found the Pico VGA near its limit already. Cycles to draw
one scanline, both cores together, on a 640x480 display, where the two
cores have about 12,800 between them:

| | avg | max |
|---|---|---|
| Two full-screen 8bpp tile planes, the costliest background measured | 7,250 | 7,560 |
| Those and mode 0 on top, every cell a different color | 10,553 | 10,911 |
| One 8bpp tile plane | 3,648 | 4,052 |
| Mode 0 alone | 3,559 | 3,832 |

That leaves almost nothing for sprites. Worse, the HIL term test at
115200 baud on that canvas loses about 1% of scanlines today, on the
640x480 display and on SXGA alike, though no byte of the stream is lost.
So three planes with a console taking text at full speed are not yet
guaranteed.

- **Why lines are lost while the console streams.** Likely core 0 away on
  the terminal's bytes while core 1 cannot draw that canvas alone. Per-line
  timestamps through the probe would show how long core 0 stays away, and
  in what. The terminal has grown since it was last measured on hardware,
  and it can be rearchitected for speed.
- **The fill modes.** Profile each per pixel and per line, costliest
  first, and make them faster: a pair table built once per run of equal
  colors, the DSP instructions, whatever the measurements point to.
- **A target**: the worst three-plane canvas, a console taking 115200 baud,
  no late scanline, and a margin for sprites.

The tools are on the local branch spike-palette: per-core cycle counters
of each scanline and of mode 0's fill, and a count of late scanlines,
which the probe reads without stopping the cores.

## 1. Spike: colors looked up as a cell is drawn

Done. It works, but it costs fill rate that is not there yet; see 0.

Each 16-bit color of a cell is told apart by the pixel format's opaque bit
(bit 5) instead of always being a resolved color:

- **Opaque**: a pixel, drawn as it is. SGR 38;2 colors are always opaque
  and are stored this way, as they are now, with no lookup.
- **Not opaque**: the other 15 bits hold an entry in a color table, the
  256 palette entries, the default foreground and the default background,
  and a bit for faint, which the renderer halves. The entry is placed so
  that one shift takes it out. Transparency comes from the table: palette
  0 and the default background, while they are the transparent black they
  start as.

The table is part of what mode 0 draws from, beside the cells, the row
table and the cursor. Bold and iCE colors still choose their entry as the
text is written, and SGR 7 still swaps the two at the cell.

What was measured, on the worst screen of 0 with every cell a different
color:

| | avg | max | late lines |
|---|---|---|---|
| Colors stored resolved, as now | 10,553 | 10,911 | 0 |
| Looked up for every cell on every scanline | 12,221 | 13,509 | 21,952 in 2 s |
| Looked up once a row, cached for its scanlines | 11,072 | 12,868 | 0 |

Each core keeps one row's resolved colors, 80 cells, and resolves them on
its first scanline of a row in a frame. That costs about 5% on that
screen, about 1,500 cycles more on the first scanline of each row, and
nothing on ordinary text, where mode 0 alone came out slightly faster
(3,436 against 3,559) because a run of cells in the same colors builds its
pixel pairs once. Resolving the whole screen once a frame instead would
cost the renderer nothing, but a line that scrolls in would show the
colors of the line it replaced for a frame, which is not acceptable.

## 2. Colors by palette entry

When the fill rate allows (0), cells keep palette entries, with the row
cache of 1:

- The color sources and the recolor pass go: 23 KB on the Pico VGA, 21.6
  KB on the Pocket, and the code that keeps them in step. A palette or
  default color change shows on the next frame, as on xterm, at no cost
  to the stream. The Pico VGA's VIDEO region gains about 1 KB for the
  table, with its faint half, and 0.65 KB for a row cache on each core:
  about 21 KB saved.
- The table joins mode0.h, and cells.c writes it as it writes the other
  words.
- mode0.sv gets a palette memory, one M10K, and looks up as many as three
  colors a cell (foreground, background, underline) in its 8 clocks. One
  fit.
- An underline color from the palette follows it too, so SGR 58 costs
  nothing more.

If the fill rate does not allow it, cells stay resolved RGB with their
sources, and SGR 58's palette underline needs a third source per cell,
about 7.5 KB on the Pico VGA and 7 KB on the Pocket, to follow a palette
change.

## 3. Kitty's underlines, all or nothing

kitty is a terminal whose author publishes a specification for each
feature, so others copy them. Its underlines spread because editors use
them: Neovim, Vim, Helix and others mark errors and warnings from their
language servers with a curly underline in a color, red for an error and
yellow for a warning. VTE, WezTerm, foot, Alacritty, Windows Terminal,
iTerm2 and mintty have them, tmux passes them through, and terminfo names
them (Smulx, Setulc).

A style is chosen with a colon parameter: 4:0 none, 4:1 single (as plain
4), 4:2 double, 4:3 curly, 4:4 dotted and 4:5 dashed. SGR 58 sets the
color and SGR 59 returns it to the foreground's. Today the terminal takes
58, 59 and every style, but draws only single and double: 4:3 to 4:5
draw as single.

All of it, or none of it:

- **All**: every style drawn, with SGR 58 and 59. Curly is a wave 3 pixel
  rows tall on 8x16 cells and 2 on 8x8, the others their patterns. A table
  of each style's pixels by glyph row serves mode0.c and mode0.sv alike.
  The style needs a third bit: the underline and double underline bits
  give four states, one of them unused today, and the attribute byte is
  full. A palette-entry color field has spare bits and a direct one does
  not, so where the bit lives is settled with 1 and 2.
- **None**: SGR 4:n, 58 and 59 go, ul_color leaves the cell, and the cell
  becomes 6 bytes: about 15 KB less on the Pico VGA and 14 KB on the
  Pocket, with a new cell layout in mode0.h, mode0.c and mode0.sv, and a
  fit.

Decided after 1 and 2.

## 4. Kitty's keyboard protocol, if 3 is all

A program that reads stdin can learn every key exactly, the same way on
other systems, without the RIA's keyboard registers. Neovim, Helix and the
fish shell use it, and foot, WezTerm, Alacritty and Ghostty have it.

- **The flags**: a program pushes, pops, sets and queries them, CSI > n u,
  CSI < u, CSI = n u and CSI ? u, on a small stack for each screen. They
  go in term.c, which the RIA's terminal shares.
- **The keys**: each is reported as CSI code ; modifiers u, the code the
  Unicode code point of the key's unshifted character in the layout, or
  one from kitty's table for the keypad, the modifiers and the other keys
  that have no character. The modifiers include Super, Hyper and Meta, and
  Caps Lock and Num Lock.
- **What the flags ask for**: telling apart keys that send the same bytes
  today (Tab and Ctrl+I, Enter and Ctrl+M, Escape and Alt), repeats and
  releases, alternate keys, every key as a sequence, and the text a key
  types. Programs often ask for the first alone, the smaller part.
  Releases need a path from the keyboard to the encoder that does not
  exist today.

vtkey.c, the key encoder, is about 220 lines and would about double. The
memory is a few bytes of flags and a table of about a hundred key codes.
About a week with its tests.

## 5. Drop Linux's palette sequences

ESC ] P nrrggbb and ESC ] R are the Linux console's, not xterm's. xterm's
OSC 4 and 104 do all they do and more: any of the 256 entries, several in
one sequence, X11 color specs, and resetting one entry or all. Without
them, ESC ] P begins an OSC like any other and runs to its BEL or ST, as on
xterm.

## 6. Bracketed paste, ?2004

While a program has it set, a paste reaches the program between
ESC [ 200 ~ and ESC [ 201 ~, so it can tell pasted text from typed text:
an editor does not indent it and a line editor does not run a pasted line.
Any ESC in the pasted text is left out, so the text cannot end the bracket.
DECRQM reports the mode and RIS clears it. Paste comes from the emulators.

## 7. XTVERSION, CSI > q

The terminal answers with its name and version, DCS > | RP6502(0.25) ST,
the version as the firmware has it but without a "v", or the CI number, so
a program that needs to know which terminal it talks to need not guess
from the device attributes.

## 8. VT420 rectangles

DECFRA (fill), DECERA (erase), DECSERA (selective erase), DECCRA (copy),
DECCARA and DECRARA (change and reverse attributes), and DECSACE choosing
stream or rectangle. xterm has them, and DECSERA completes the protected
areas.

## 9. Left and right margins

DECSLRM, with ?69 (DECLRMM) to allow it. tmux and some editors use them
when the terminal has them. Every insert, delete and scroll learns the
margins.

## 10. Drop the transparent color, SGR 38:1 and 48:1

Palette entry 0 is transparent black until a program changes it, and the
default background starts as it, so text over a picture on a plane
behind the console shows the picture wherever its background is 0 or the
default. A program that changes entry 0 with OSC 4 puts a color there and
gets an opaque one, which is what it expects. A program that wants to be
seen through follows those rules and needs nothing more, so SGR 38:1 and
48:1, which no other terminal draws and which mintty, foot and iTerm2
read as bold in the 38;1 form, go. 38:1 and 48:1 become forms SGR does
not know, taken whole like any other.

## 11. The `CSI t` window reports

xterm's, from dtterm, and kept. The rest of `CSI t` moves, resizes and
raises a desktop window, and is ignored. The reports:

- **11**, whether the window is iconified: always no on the Pico VGA and
  the Pocket. The emulator can answer yes while its window is minimized
  (Sokol tells it, with its iconified and restored events), and the web
  player while its page is hidden. If that is awkward on a host, that host
  answers no.
- **14**, the text area in pixels, and **16**, a character cell in
  pixels: how text lines up with pixels, which a program drawing on the
  planes behind the console can use.
- **18**, the text area in characters, and **19**, the screen in
  characters: the size the terminal is shown at, or the size a program set
  with RIA_ATTR_RLN_WIDTH and HEIGHT. There is no larger screen for the
  terminal to grow into, so the two always answer the same, as they do
  now.

## 12. Drop SGR 10 and 11

The Linux console took SGR 11, ECMA-48's first alternate font, to mean
"draw the controls": bytes 1 to 31 and DEL print as their code page's
pictures, such as the card suits and arrows of CP437, instead of acting.
SGR 10 turns that off. ECMA-48's own way is CRM, CSI 3 h and CSI 3 l, which
the terminal has and which sets the same switch, so SGR 10 and 11 go. The
target is xterm, not the Linux console.

## 13. Smooth scroll, DECSCLM (?4)

An experiment, to see before deciding. A VT100 scrolled smoothly at 6
lines a second and stopped the host with XOFF when it fell behind. There is
no flow control here, so what arrives during a scroll has to wait
somewhere, and at 115200 baud a screen of text arrives in about 0.2 s. The
renderer would draw the scroll region at a pixel offset, which the row
table could carry. A demo ROM shows it on the emulator and on a Pico.

## 14. The manual

- The Terminal section of term.rst opens with what the terminal is for:
  the VT102 apart from 132 columns, the xterm subset for 256 colors and
  what ported programs send, the ANSI-BBS conventions, and kitty's
  underlines and keyboard protocol if 3 and 4 land. Each is there because
  it is worth having, and the rest of the section follows from it. It does
  not list what a VT102's hardware did that this machine cannot.
- term.rst lists fast blink (SGR 6) and overline (SGR 53, 55) as features
  from other terminals; both are ECMA-48's. The list loses them and gains
  xterm's, aixterm's and SCO's.
- With 10, the manual loses SGR 38:1 and 48:1, and says how to be seen
  through: palette 0 and the default background, unchanged.
- Every item above that lands is documented with it.

## Not doing

- **kitty's graphics protocol.** The terminal keeps the images a program
  sends, often megabytes, and draws them into the text. There is not the
  memory, and the machine draws pictures on its own planes.
- **kitty's other protocols**: desktop notifications, color control by
  name (OSC 21, which overlaps xterm's OSC 4, 10 and 11), and text in
  larger sizes. Few programs use them.
- **Synchronized output, ?2026.** The cells are drawn as they are written,
  with no second copy, so text written over text already showing cannot be
  held back. Holding back only the row table and the cursor would be a
  partial version, and a program that asks for the whole would be misled.
- **What needs a real terminal**: 132 columns, interlace, the keyboard
  LEDs, the printer and the answerback.

## Ways to need less memory

To work out with the plan, not after it.

- Colors by palette entry (1 and 2) take the color sources away.
- Dropping kitty's underlines (3) takes a quarter of each cell.
- The 40- and 80-column terminals both keep their cells all the time, and
  so do their alternate screens. Rebuilding one size from the other on a
  canvas change would halve the cells, if what is lost in the rebuild is
  acceptable.
- src/core/harvest.md lists memory to win back everywhere else.
