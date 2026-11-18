# The console: work still to do

Each section is done properly: where the architecture is what is in the
way, the architecture changes.

## 1. A clear line after a program ends

When a program ends, the console is given a soft reset and then a line of
its own: a CR LF if the cursor is not in the first column
(`com_end_reset`). That is not always a clear line. A program that drew
below its cursor leaves the monitor's prompt on top of what it drew.

After the soft reset, the cursor's row and every row below it must be
clear. If they are not, the cursor goes to the row after the last one in
use: a CR, and an LF for each row down. Line feeds mean the same on a
terminal of any size, and scroll at the bottom, which is what makes a
clear row when the last one is in use. The cursor never moves up.

A row is in use when it has a character other than a space, or a cell
whose background is not the default. The cell layer answers for a row:

- A machine with the cells looks at them.
- The RP6502-RIA has no cells. Its stub keeps a flag for each row,
  about 60 bytes a screen, in the calls it is already given: set by a
  character or a background, cleared when the whole row is erased or
  scrolls in, and moved with the rows.

The RP6502-RIA's flags are a comfort and not exact. A row that a program
blanked with spaces, or erased part of, still counts as in use, and the
prompt is then lower than it had to be. That is the right way to be
wrong: there is always a clear line.

## 2. The resets at a cold boot

What the RP6502-RIA sends as it starts, today:

- A hard reset, RIS, to the screen's terminal alone, when the VGA
  connects (ria/sys/vga.c). It goes over PIX, so the VGA's USB port, a
  terminal on the UART, and telnet do not get it and keep what they show.
- Then the monitor's banner, which starts with the soft reset, to every
  terminal (ria/mon/mon.c).

That is what is wanted, in either order, but for one thing: a build that
logs. Its log is on the screen's terminal, and the hard reset clears it.

A build that logs is one with any log level above NONE: a Debug build,
whose level is ERROR, or any build given one with `RP6502_LOG_LEVEL` or
`RP6502_LOG_LEVELS`. In such a build the screen's terminal gets no hard
reset, only the soft one every terminal gets. A build that logs nothing
always gives it the hard reset.

Whatever the VGA's terminal is sent or not sent, the RP6502-RIA's copy of
it gets the same, as it does now: a byte for the screen alone goes to both
from one place (`manifold_screen_write`), and the DISPLAY, code page, and
mode 0 registers are followed in ria/sys/vga.c. The hard reset that a
build that logs leaves out is left out of both.

## 3. The whole line editor in VT52 mode

A read that starts with the terminal in VT52 mode is laid out plainly:
the line editor echoes, backs up with BS, erases with spaces, and leaves
long lines for the terminal to wrap (core/com/rln.c). A VT52 does not
wrap. Its cursor stays on the last column.

A VT52 has every command the laid-out line needs, each without a count:

| The line editor sends | In VT52 mode |
|---|---|
| `CSI n A`, up | `ESC A`, n times |
| `CSI n B`, down | `ESC B`, n times |
| `CSI n C`, right | `ESC C`, n times |
| `CSI K`, erase to the end of the line | `ESC K` |
| `CSI 4 l`, insert mode off | nothing: a VT52 has no insert mode |
| the cursor's shape, to the screen alone | nothing |

So VT52 mode stops making a line plain, and the line editor sends the
VT52's form of each where the terminal is in that mode. The keys need
nothing: `ESC A` to `ESC D`, `ESC H`, and `ESC F` are already read as a
VT52's arrows, Home, and End.

A plain line stays for what asks for one by name: a script's read, and
the command line emulator when its output is a pipe or a file and not a
terminal (`rln_set_plain`), which no other machine sets.

## 4. The line editor inside margins

The line editor lays a line out for the whole width of the cursor's row,
and goes to a column with a CR and a move right (core/com/rln.c). It does
not know about margins. Inside left and right margins the terminal wraps
at the right margin, before the line editor's own break, and a CR lands
on the left margin and not on column 1, so the line editor and the
screen disagree about where the line is. It happens only to a read made
while a program has margins set: the soft reset at a program's end
clears them before the monitor reads.

The line editor asks the terminal for the space the cursor is in:

- The columns of a row are those between the left and right margins when
  the cursor is inside them, and the whole row when it is not. The first
  is where a CR lands. grid.c already answers both for the cursor's own
  moves (`grid_left_stop`, `grid_right_stop`, `grid_cursor_in_margins`).
- The most rows a line may take is no more than the scroll region has,
  as well as no more than the screen has. Moves up and down are relative
  and a CR LF at the bottom margin scrolls the region, so nothing else of
  top and bottom margins needs care.

The attributes for the terminal's width and height stay the terminal's
size, 0 for the size of the one showing, as every system has a window's
size whatever margins are set in it. A program that set margins knows
them. A size a program gives in them, other than 0, is the line editor's
size and the margins are those of that terminal, as now.
