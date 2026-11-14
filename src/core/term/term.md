# The console terminal: work still to do

The terminal is three files: term.c (the parser and everything the same at
every size), grid.c (the terminal at one size: cursor, margins, tabs, line
sizes) and cells.c (the screen's cells, for a machine that draws). This is
what is left.

## 1. The Pocket's renderer: double-size lines and the reverse screen

The Pocket's software already passes a row's size in bits 16-17 of VID_ROW
and DECSCNM in bit 26 of VID_CURSOR. mode0.sv has to draw them (Quartus):

- **Reverse screen:** swap every cell's foreground and background.
- **Double width:** draw the row's first half of cells, every pixel twice
  across.
- **Double height:** as double width, drawing the top or bottom half of each
  glyph with every glyph row twice.

## 2. Recolor and protected areas, and the Pocket memory for them

- **Recolor text already on screen** when OSC 4, 104, 10, 11 or the Linux
  palette sequences change a color, as xterm and Linux do.
- **Protected areas and selective erase:** DECSCA with DECSED and DECSEL;
  ECMA-48's SPA, EPA and ERM.

Both need more per cell than the 8 bytes the renderers read. The plan is a
second table in cells.c, 3 bytes a cell in CPU RAM, indexed by the same
physical row: the foreground's and background's source (palette entry,
default or RGB, 9 bits each), faint, protected, and whether the underline
follows the foreground. The renderers and the FPGA don't change.

| | Renderer table | cells.c table |
|---|---|---|
| Pico VGA, 7,680 cells | 61.4 KB in VIDEO, as now | 23 KB in lower RAM (86 KB free) |
| Pocket, 7,200 cells | 57.6 KB in VIDCELLS, as now | 21.6 KB of the soft CPU's 96 KB |

The Pocket needs about 14 KB more free than the ~7 KB it has. Ideas, each
with what it saves:

- **Our own time library** on every platform, with only what we use: no
  `strftime`, `tzset` (and the `sscanf` it pulls in), `mktime`, `localtime`,
  `gmtime` or `setenv`; 32-bit day arithmetic. About 6 KB.
- **`mode2_options`** once per program instead of per scanline and plane.
  About 3 KB.
- **`api_return_errno`** as one function instead of eight inline copies.
  About 2.2 KB.
- **The line editor's history** serves only the monitor, which the Pocket
  doesn't have. 2 KB.
- **64-bit divides** in the APF, HID and clock code. About 1.7 KB.
- **Buffers:** com.c's 1 KB buffer and the file system's pool. 1-2 KB.

## 3. The terminal on the RIA, and the keyboard modes

### Step 2: the RIA follows the terminal and answers queries

- The RIA builds term.c and grid.c, with its own cells.h functions: replies
  through a reply source and held buffer of its own (like core/com's
  manifold_reply, not COM_SOURCE_UART), everything else empty. It defines
  TERM_MAX_HEIGHT 32. The terminal is skipped while the RIA flashes itself.
- **Feed it where bytes go to the VGA** (manifold_vga_write), not where they
  are queued, and apply each xreg at its own pix_send. With no VGA, feed it
  the UART output, including the screen-only bytes.
- **Sizes:** replay the VGA's term_set_height calls in the same order:
  MODE 0 (record its BEGIN and END xregs; apply only when the VGA acks),
  CANVAS to the console when the old canvas wasn't, and DISPLAY (zero to two
  calls, depending on view switching). Same layout code, mode0_layout.
- **Code page:** track the VGA's page with font.c's list to know when a
  change does a RIS. Make the Pocket's font_set_code_page do the same.
- **VGA connect** (only at a program's start): RIS to the screen, resend
  DISPLAY and CODE_PAGE, reset the RIA's terminal. Debug builds too.
- **Attribute 0x0D:** 1 means the RIA doesn't answer queries. Set at each
  program start: 0 with a VGA, 1 without. A program may set either.
- **Attributes 0x0A and 0x0B** (RLN_WIDTH, RLN_HEIGHT): 0 means the
  screen's size (80x24 with no VGA); reading gives the size in use. Non-zero
  adds a third grid, up to 255x255: copy the showing grid (repointing its
  cur and ls, setting tab stops past the old width, resetting the margins),
  then resize. The line editor and the replies use it while set. Dropped
  when both are 0 again. Needs a way to add and drop a grid on a running
  terminal.
- **Window reports:** the pixel cell size comes from the machine, next to
  cells_shown, not from the width. REP's cap: a multiple of both row lengths
  for odd widths.
- **Remove:** the VGA's reply path and SUPPRESS_TERM_REPLY ($1:F:02).
- Firmware updates must work with either chip updated first.

### Step 3: the line editor stops probing

- No CPR, no DA2, no size query. The prompt column, width and height come
  from the RIA's terminal when a read starts, after everything printed before
  the read has reached it. The prompt column counts a pending wrap and the
  row's line size.
- Break rows with CR LF right after the last column; move between rows with
  cursor up and down; clear a redrawn row with EL. rln_resize_redraw no
  longer assumes the terminal reflowed. TIO_MODES keeps insert mode off and
  drops ?7h.
- Cursor shapes go only to the screen, over the screen-only channel.
- Remove: the probes and their wait, tio's per-terminal marks, answers and
  greetings, the answer-only-while-owed rule, absorbing CR LF before the size
  wait, the ranking of which terminal is in charge, NAWS as a size, and the
  manifold's CR/LF clean-line tracking (use "cursor in column 1" instead).
- Emulator headless: stdout to a pipe or file, the line editor types plainly
  (echo and editing keys, no width). stdout to a terminal, the terminal
  doesn't answer queries; the line editor still uses the terminal's size.
  core keeps manifold_set_term_in_charge and the reply switch for this.
  console.c and console_roles.py change with it.
- Docs: term.rst and api.rst (0x0A, 0x0B, 0x0D), and msbasic's pager.

### Step 4: keyboard modes

The RIA's keyboard reads the terminal: DECCKM `?1`, DECKPAM `ESC =` and
DECKPNM `ESC >`, LNM's Return-sends-CR-LF, DECARM `?8`, DECLL `CSI q`, and
VT52 mode's arrows.

- keymap.c (RIA, Pocket) and vtkeys.c (emulators) read the terminal.
  Rework core/hid as needed.
- DECLL needs a lamp byte apart from keyboard_hid_leds (KEYB SST change).
- Libretro has no repeat flag, so no DECARM there. Sokol turns keypad digits
  into navigation and sends keypad operators as characters.
- VT52 keypad keys (ESC ? p..y) need framing in tio; rln needs ESC A-D.
- vttest's harness types through vtkeys so the keyboard tests run.

## 4. Mouse

After section 3. xterm's `?9` and `?1000` to `?1006`, maybe the DEC locator;
which ones is decided then. Its modes are more terminal state on the RIA.
Once `?1001` exists, `CSI 1;x;y;10;20 T` has to be told apart from SD.
