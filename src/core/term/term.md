# The console terminal: things to explore

The terminal is three files: term.c (the parser and everything the same at
every size), grid.c (the terminal at one size: cursor, margins, tabs, line
sizes) and cells.c (the screen's cells and their bookkeeping). cells.c
writes what mode 0 draws from, which core/vga/mode/mode0.h defines.

Nothing here is decided. It is the list to think about, together, before
any of it is built, because several items cost memory.

## The target

Three layers, and anything outside them has to earn its place:

1. **The VT102**, apart from what needs a real terminal's hardware: 132
   columns, the keyboard LEDs (DECLL), the printer (MC), the answerback,
   interlace and the alternate ROM character sets. Compliant enough; bugs
   get fixed as they are found.
2. **The xterm subset that `xterm-256color` terminfo uses**, which is what
   ported curses code and remote hosts send.
3. **ANSI-BBS**: CP437, iCE colors and SyncTERM's conventions.

## Memory now

| | Cells (mode0_cells) | Color sources (cells_srcs) | Free |
|---|---|---|---|
| Pico VGA | 61.4 KB in VIDEO | 23 KB in lower RAM | about 66 KB of lower RAM, before the stacks and heap |
| Pocket | 57.6 KB in VIDCELLS | 21.6 KB of the 64 KB RAM block | about 21 KB of the RAM block, unused by any heap; 192 KB more in its window for the block to grow |

Both sizes, 40 and 80 columns, follow every byte at once, and both have an
alternate screen, so every per-cell table is paid four times over.

## Decisions

### SGR 58's palette underline, or kitty's underlines at all

SGR 58 is kitty's. Every terminal that has it but mintty stores a palette
underline (58;5;n) as the entry, so OSC 4 and 104 recolor it. Ours keeps
the color it was drawn in. Following the palette needs a third source per
cell: about 7.5 KB on the Pico VGA and 7 KB on the Pocket. The other choice
is to drop SGR 58 and 59 and kitty's underline styles (4:n). That would
leave ul_color, 2 of each cell's 8 bytes, unused, but the fabric reads a
cell as two 32-bit words, so getting those bytes back means a new cell
layout in mode0.h, mode0.c and mode0.sv, and a fit.

### The extensions outside the target

Each one: keep it, or drop it.

- **kitty's underline color and styles**: as above.
- **Linux's palette sequences**, ESC ] P and ESC ] R: the Linux console's;
  xterm does not have them. They are one more way to set a palette entry
  and cost nothing in memory. They are not why SGR 58 would need memory:
  any palette change, OSC 4 included, needs that.
- **SGR 10 and 11**, the Linux and SCO meanings, draw the controls or not.
- **The `CSI t` window reports** 11, 14, 16, 18 and 19, which describe a
  window this machine does not have.
- **The transparent color, SGR 38:1 and 48:1**: no other terminal does
  anything with it, but ITU T.416 defines color type 1 as transparent, so
  it is the standard's spelling. The semicolon form 38;1 shows bold on
  mintty, foot and iTerm2, which take the 1 as SGR 1. Keep both forms, or
  the colon form alone. The manual calls it "the color as it is, made
  transparent"; T.416's transparent is the whole color, and the manual
  should say that.

### The manual's list of borrowed features

term.rst lists fast blink (SGR 6) and overline (SGR 53, 55) as features
from other terminals; both are ECMA-48's. The list should lose them and
gain xterm's, aixterm's, Linux's and SCO's.

## Experiments

### Smooth scroll, DECSCLM (?4)

Today it is accepted and ignored. A VT100 scrolled smoothly at 6 lines a
second and stopped the host with XOFF when it fell behind. There is no flow
control here, so the bytes that arrive during a scroll have to wait
somewhere, and at 115200 baud a screen of text arrives in about 0.2 s. The
renderer would draw the scroll region at a pixel offset, which the row
table could carry. It has to be seen on the emulator and on a Pico, with a
demo ROM made to show it, before anything is decided.

### Synchronized output, ?2026

A program brackets an update, and the terminal shows none of it until it
ends, so a redraw never shows half done. The cells are drawn as they are
written, with no second copy, so holding back mode 0's words at vblank
(the row table, the cursor and the screen word) would hold back scrolls,
cursor moves and screen switches, but not text written over text already
showing. Whether that partial form is worth having, or misleads a program
that relies on the whole, is the question.

## Additions

- **VT420 rectangles**: DECFRA (fill), DECERA (erase), DECSERA (selective
  erase), DECCRA (copy), DECCARA and DECRARA (change and reverse
  attributes), with DECSACE choosing stream or rectangle. xterm has them,
  and DECSERA completes the protected areas. No memory.
- **Left and right margins**, DECSLRM with ?69 (DECLRMM): tmux and some
  editors use them when the terminal has them. A few bytes a grid, and
  every insert, delete and scroll learns the margins.
- **XTVERSION**, CSI > q: the terminal answers with its name and version,
  DCS > | name version ST, so a program that needs to know exactly which
  terminal it talks to need not guess from the device attributes. No
  memory.
- **Bracketed paste**, ?2004: while a program has it set, a paste reaches
  the program between ESC [ 200 ~ and ESC [ 201 ~, so it can tell pasted
  text from typed text: an editor does not auto-indent it, and a shell or
  the line editor does not run a pasted line. It belongs with copy, paste
  and the mouse, as their own batch of work.

## Ways to need less memory

To work out with the list above, not after it.

- The 40- and 80-column terminals both keep their cells all the time, and
  so do their alternate screens. Rebuilding one size from the other on a
  canvas change would halve the cells and the sources, if what is lost
  in the rebuild is acceptable.
- A cell's colors are resolved RGB, 6 bytes of its 8, so it can be drawn
  without a lookup. Cells that kept palette entries and resolved them at
  render time would need no source table, but would change the renderers
  and the fabric.
- src/core/harvest.md lists memory to win back everywhere else.
