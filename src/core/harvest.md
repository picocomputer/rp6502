# The console: work still to do

Each section is done properly: where the architecture is what is in the
way, the architecture changes.

## 1. The line editor inside margins

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
