# The console terminal: work still to do

term.c is a VT102 with most of xterm and the Linux console. This is what is
left, kept here until it is done. Each item says what it is and what it takes.

Two facts shape the work:

- **A cell has no spare bits.** A cell is 8 bytes: the glyph, 8 bits of
  attributes, and the foreground, background and underline colors. All eight
  attribute bits are used. The Pico renderer (core/vga/mode/mode0.c) and the
  Pocket's FPGA both read cells straight from memory, so a change to the cell
  is a change to both renderers, and the FPGA's is an RTL change built with
  Quartus.
- **Keys are made on the RIA; the screen is drawn on the VGA.** The keyboard
  is read and turned into bytes on the RIA, while term.c draws on the VGA.
  Section 3 gives the RIA its own copy of the terminal's state.

## 1. The Pocket's renderer: double-size lines and the reverse screen

DECALN, SL/SR, the reverse screen (DECSCNM) and double-width and double-height
lines are done in term.c and the Pico renderer. The Pocket's software already
passes the two new pieces to the fabric: a row's size (TERM_LINE_) in bits
16-17 of its VID_ROW word, and DECSCNM in bit 26 of VID_CURSOR. mode0.sv still
has to draw them, which needs the next Quartus build:

- **Reverse screen:** swap every cell's foreground and background.
- **Double width:** draw the row's first half of cells, every pixel twice
  across.
- **Double height:** as double width, drawing the top or bottom half of each
  glyph with every glyph row twice.

## 2. Needs bigger cells, and the memory for them

- **Recolor text already on screen** when OSC 4, 104, 10, 11 or the Linux
  palette sequences change a color, as xterm and Linux do. Today only new text
  takes the new color. A cell keeps where its colors came from, and a color
  change starts a background pass, one row a tick, rewriting the cells that use
  it.
- **Protected areas and selective erase.** DECSCA marks characters that the
  selective erases (DECSED, DECSEL) leave alone; ECMA-48's SPA, EPA and ERM do
  the same. Used for DEC form screens. One bit per cell.

**The plan:** make cells bigger rather than keep a separate table, and only go
back to a separate table if this fails. A cell keeps its 8 bytes for the
renderers and gains 29 bits:

- where the foreground, background and underline colors came from: a palette
  entry 0-255, the default, or RGB (9 bits each);
- faint (1 bit), so a recolored color can be halved again;
- protected (1 bit).

That makes a 12-byte cell, which stays word-aligned for the renderers. Both
terminals stay, 40 and 80 columns, each with its alternate screen.

**Splitting the cell into two tables (explored 2026-11-13).** term.c only
writes cells; it never reads one back except to move it (DCH, ICH, insert
mode, SL/SR). The renderers read all 8 bytes: the glyph, every attribute bit,
both colors, and the underline color on underline rows. So nothing in today's
cell can leave the scanout memory. Everything new is term.c-only, which gives a
clean split:

- **Renderer table:** today's 8-byte cell, unchanged. It stays in the scanout
  memory (VIDEO on the Pico, VIDCELLS on the Pocket), and the FPGA needs no
  change.
- **term.c table, 3 bytes a cell, in CPU RAM:** the foreground's source and
  the background's (a palette entry 0-255, default, or RGB: 9 bits each),
  faint, protected, and whether the underline color follows the foreground
  (so a recolor rewrites it too; an explicit SGR 58;5;N stays). 21 bits.
  A 4th byte would let SGR 58;5;N recolor as well.

Both tables share the scroll remap, since they are indexed by the same
physical row. Every glyph write, erase and move writes both.

| | Renderer table | term.c table (3 bytes) |
|---|---|---|
| Pico VGA, 7,680 cells | 61.4 KB in VIDEO, as now | 23 KB in lower RAM, which has 86 KB free |
| Pocket, 7,200 cells | 57.6 KB in VIDCELLS, as now | 21.6 KB in the soft CPU's 96 KB |

With the split the Pocket needs no RTL change and no more VIDCELLS for recolor
and protected areas; it needs about 14 KB more free in the soft CPU's RAM than
the ~7.8 KB it has today, which the memory work below can provide. 12-byte
cells instead need 25 KB more VIDCELLS, which means freeing 32 KB of the soft
CPU's M10K blocks and an RTL change. The split looks like the better plan.

**The Pico VGA** has room: its cells grow from 61 KB to 92 KB, and the video
region can grow down into free RAM and stay in the upper banks.

**The Pocket** does not. Its cells live in FPGA block memory (VIDCELLS, 61,440
bytes), 12-byte cells need 86.4 KB, and the FPGA has only about five M10K
blocks left. The memory has to come from the soft CPU: its 96 KB are M10K
blocks too, and getting it down to 64 KB frees 32 KB for VIDCELLS (an RTL
change).

Today the soft CPU uses 70.6 KB of code, 15.8 KB of data and a 4 KB stack:
90.5 KB. Getting to 64 KB means cutting about 26.5 KB. Measured so far, building
with `ninja sw_bin` only:

| Subsystem | Code | Data |
|---|---|---|
| libc and libgcc | 11.4 KB | |
| term.c | 9.6 KB | 1.3 KB |
| the main loop, with the drivers inlined into it | 7.1 KB | |
| APF file slots | 4.8 KB | |
| API | 4.1 KB | |
| line editor | 4.0 KB | 2.6 KB |
| file system | 3.9 KB | 2.3 KB |
| mode 2 options | | 3.1 KB |

Ideas, with what each would save:

- **Our own time library, the same on every platform,** with only what we use:
  no `strftime` from libc, no `tzset` and the `sscanf` it alone pulls in, no
  `mktime`, `localtime` or `gmtime`, and no `setenv` to pass a timezone. 32-bit
  day arithmetic also drops the 64-bit modulo. About 6 KB.
- **`mode2_options`** is kept for every scanline and plane. If the Pocket only
  needs it once per program, about 3 KB.
- **`api_return_errno`** is a `static inline` in api.h, and the Pocket build
  has eight copies of its 320 bytes. As one function, about 2.2 KB.
- **The line editor's history** serves only the monitor, which the Pocket does
  not have. 2 KB.
- **64-bit divides** in the APF, HID and clock code pull in libgcc's 64-bit
  division. About 1.7 KB.
- **Buffers:** com.c's 1 KB buffer and the file system's pool. 1-2 KB, once
  their sizes are checked.

That is about 16-17 KB. The rest has to come from term.c, the drivers inlined
into the main loop and the APF code, measured as we go.

Speed doesn't matter on the soft CPU: it runs at 50 MHz and mostly copies
bytes. It already builds with `-Os`; `-Oz` gives the same binary with this GCC.
The `O3` pragmas in the mode and audio files cost only about 250 bytes there,
since their renderers are not built for the Pocket. A function that does need
speed can be tagged on its own.

## 3. Next: one terminal model on both chips, and the keyboard modes

Decided 2026-11-13. term.c splits in two:

- **The model:** the parser, the cursor, every mode, the margins, the tab
  stops, the saved cursors, the alternate screen, and each row's line size and
  scrolling (a double-width row changes how far the cursor goes). No cells and
  no colors. It also answers the terminal's reports: CPR, DSR, DA, DA2, DA3,
  DECRQM and the window reports.
- **The cell writer:** glyphs, colors, erases and moves, driven by the model.

The VGA builds both and never replies. The RIA builds the model alone, for
both terminals (40 and 80 columns), and feeds it the console's output stream,
the same bytes the VGA gets. The two stay in step because they are the same
code fed the same bytes. The emulators and the Pocket run term.c in one
program, so there the model is term.c's own state and nothing is duplicated.

What it gives:

- **Replies come from the RIA**, in the stream's order and with no round trip.
  The VGA's reply
  path and the SUPPRESS_TERM_REPLY register go away. The screen-only channel
  stays for the resets, which differ between the screen and the serial port.
- **The line editor reads the model** for the screen terminal's cursor column
  and size instead of sending CPR and DA2 and waiting.
- **The keyboard modes are model state the RIA's keyboard reads:**
  - cursor-key mode, DECCKM `?1`: the arrows send `ESC O A` for `ESC [ A`;
  - keypad modes, DECKPAM `ESC =` and DECKPNM `ESC >`;
  - LNM's keyboard half, `CSI 20 h`: Return sends CR LF;
  - autorepeat, DECARM `?8`;
  - the keyboard LEDs, DECLL `CSI q`;
  - VT52 mode, where the arrows send `ESC A` instead of `ESC [ A`.

  A terminal on the UART or telnet handles its own keys from the same stream,
  which is already right.

The line editor and the console, decided 2026-11-13:

- **The screen's terminal is always the one in charge.** The line editor never
  sends CPR or DA2 and never asks for a size. The prompt column, the width and
  the height come from the model when a read starts. The line editor waits
  until everything printed before the read has reached the model (see "Where
  the model is fed" below). A RIA with no VGA plugged in still has the model.
- **The line editor breaks its rows itself**, with CR LF at the screen's width,
  instead of relying on the terminal's autowrap. Moves between its own rows are
  cursor up and down, and a row it redraws is cleared with EL. On term.c a CR
  LF from a pending wrap lands where the wrap would have, so the screen looks
  the same; a wider telnet or UART terminal sees a clean 80-column editor. A
  narrower one garbles, which is accepted: those terminals echo the screen.
- **Cursor shapes go to term.c only**, over the screen-only channel, never to
  the UART or telnet.
- **Gone:** the CPR and DA2 probes and the wait for them; tio's per-terminal
  marks, answers and greetings; the rule that an R-final CSI is an answer only
  while one is owed; absorbing CR LF before the size wait; the ranking of which
  terminal is in charge; NAWS as a size (dropped for now; it can come back);
  the manifold's CR/LF clean-line tracking, replaced by asking the
  model whether the screen's cursor is in column 1. A program that exits with
  text left of the cursor gets it overwritten by the prompt, which clears its
  row; that is accepted.
- **RIA_ATTR_RLN_WIDTH (0x0A) and RIA_ATTR_RLN_HEIGHT (0x0B) stay.** A
  program starts with both at 0, meaning the model's own size: the VGA's
  console, or 80x24 with no VGA. Reading one gives the size in use, never 0.
  Setting one to a non-zero value sets the model's width or height, so the
  line editor and the replies use it from then on. The screen's term.c keeps
  its real size, so a program that sets 132 columns gets a 132-column line
  editor and a garbled screen; that is the program's choice. Setting 0 goes
  back to the real size. The RIA's model has to allow widths up to 255.
- **The model answers a program's queries unless attribute 0x0D is 1.** When
  a program starts, the RIA sets it to 0 if a VGA is connected and to 1 if
  not, so it also tells a program whether it has a VGA. A program sets it to
  1 to talk to a terminal of its own on the UART or telnet, or to 0 to have
  the model answer with no VGA. It matters only while a program runs. If
  another terminal answers too, the program gets both; that is the user's
  setup to manage. This replaces SUPPRESS_TERM_REPLY.
- **The emulator's headless modes**, decided in its console code alone:
  - stdout on a pipe or a file: the line editor types plainly. It still echoes
    and acts on editing keys and their cursor moves, but formats nothing for a
    width. For scripts and logs.
  - stdout on a terminal: that terminal is the screen the user sees, and it
    answers queries, so term.c's model does not. The line editor still lays
    out for the model's size, not the host terminal's.

Keeping the two copies in step:

- **Where the model is fed.** Bytes at the point they go to the VGA
  (manifold_vga_write in ria/com/manifold.c), not when they are queued, and
  each xreg at its own pix_send. MODE, CANVAS, DISPLAY and CODE_PAGE go out
  over PIX and can overtake bytes still in the console rings, so this is the
  only point where both chips see the same order. With no VGA the tap is the
  UART output, and it also takes the screen-only bytes dropped today.
- **Size changes.** term_set_height runs on MODE 0 (its BEGIN and END come
  from xregs the RIA must now record), on CANVAS to the console when the old
  canvas wasn't the console, and on DISPLAY, which reprograms the console zero
  to two times depending on the VGA's view switching. Each step moves the
  cursor differently and resets the margins, so the RIA replays the same
  calls in the same order through the same layout code (mode0_layout), not
  just the final height.
- **The VGA restarting on its own** (a power cycle while the RIA runs): its
  terminal comes back fresh and the RIA's model doesn't. A VGA only connects
  when a program starts. On connect the RIA sends a full reset (RIS) to the
  screen, resends DISPLAY and CODE_PAGE, and resets its model, so both start
  from the same state. RIS doesn't touch the height, so the resend has to.
  Debug builds need this too; today the boot reset is NDEBUG only.
- Code page changes and the end-of-program reset come from the RIA, which
  applies them to its model itself.
- Lost UART bytes would also break the step; the HIL term test shows none at
  115200.

Notes from the plan review, 2026-11-13:

- **The pen.** The model keeps the unresolved pen (palette entry, default or
  RGB, and the attributes) in its cursors, so DECSC/DECRC, ?1048, ?1049, the
  alternate screen, DECSTR and RIS copy it with the cursor as today. The cell
  writer resolves colors when it writes.
- **The prompt column** accounts for a pending wrap (a prompt ending on the
  last column puts the first character on the next row) and for the row's
  line size (a double-width row has half the columns).
- **Row breaks.** rln sends CR LF right after the last column, so a wider
  terminal never sees the cursor parked in a pending wrap; its relative moves
  then land right everywhere. rln_resize_redraw no longer assumes the
  terminal reflowed the line.
- **TIO_MODES** still turns insert mode off, since rln writes over its own
  text, and stops sending ?7h.
- **Replies on the RIA** need a source of their own and a held buffer, like
  core/com/manifold.c's in-process manifold_reply, not COM_SOURCE_UART.
- **Self-flashing.** The console keeps running from RAM while the RIA
  rewrites its flash. The model is skipped while flashing; a reboot follows.
  The RIA defines TERM_MAX_HEIGHT 32.
- **Headless.** core keeps manifold_set_term_in_charge and the reply switch,
  which console.c uses for its two modes. console.c and console_roles.py
  expect probing today and change with it.
- **Keyboard.** keymap.c (RIA, Pocket) and vtkeys.c (emulators) read the
  model. DECLL needs a lamp byte of its own, apart from the lock state in
  keyboard_hid_leds (a KEYB SST change). Libretro has no repeat flag, so
  DECARM can't apply there. Sokol turns keypad digits into navigation and
  sends keypad operators as characters. VT52 keypad keys (ESC ? p..y) need
  framing in tio, and rln needs the VT52 arrows ESC A-D.
- **Code page.** The RIA tracks the VGA's page with font.c's list to know when
  a change does a RIS. The Pocket's font_set_code_page doesn't RIS; make the
  two agree.
- **Build.** The VGA linker puts term.c's .bss in VIDEO and builds it without
  LTO; the Pocket places cells by array name. Splitting the file moves these.
  The TERM, TIO_ and MFLD SST chunks and state_inventory.txt change. The
  split's code growth on the Pocket counts against section 2's cut.
- **Mixed firmware** isn't supported; the boot screen shows both versions.
  Upgrading one chip and then the other has to work in either order.

The keyboard code (core/hid: keyboard, keymap, vtkeys, and the host and OSAL
parts) gets whatever architectural change this needs.

Testing: vttest's keyboard tests need their keys to go through vtkeys so the
modes can change them. The vttest harness (tests/host/vttest/vtrun.c) writes
keys straight to vttest today, and would type through vtkeys instead.

## 4. Mouse

Deferred until after section 3.

- **Mouse reporting,** the xterm modes `?9` and `?1000` to `?1006`, and maybe
  the DEC locator from the VT330/340. A program asks for clicks, drags or
  motion, and the terminal sends them as escape sequences. Which protocols to
  support is decided when this starts. The mouse is on the RIA, so its modes
  become more state in the shared model, like the keyboard modes.
- Once `?1001` exists, `CSI 1;x;y;10;20 T` has to be told apart from SD.
- Selecting text on the screen locally is a separate feature, which we don't
  have.
