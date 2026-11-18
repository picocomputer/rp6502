# Harvest memory: work still to do

Memory that comes back on every machine, to be done on its own branch.
No machine is short of memory now; the Pocket's soft CPU has its RAM in
SDRAM. This work keeps every machine lean and its code plain.

Each section is done properly: where the architecture is what keeps the
memory, the architecture changes, and no section is a link-time trick.

## 1. The RTL tests on CI

Not memory, but waiting on it holds up every branch, so it goes first
and CI runs while the video work is done. CI runs the RTL tests as two
jobs, A and B, each on its own runner. Both build the whole simulation,
and `ctest -I` gives A the odd tests and B the even ones. A can take
far longer than B.

The run of 2026-11-18 says it was the runner, mostly. A took 36 minutes
and B took 14:

| | A | B |
|---|---|---|
| Installing packages | 18 min 29 s | 22 s |
| Building | 8 min 31 s | 7 min 38 s |
| Testing | 9 min 19 s | 5 min 40 s |

A's runner fetched the 232 MB of packages at 215 kB/s. It was also slow
at the tests: they took 1.5 times the processor time they take on the
bench, where B's took 1.1 times. The halves are uneven too, but by
less: on the bench A's tests are 1,449 s of processor time and B's
1,191 s, because A has the three longest (aud, opl.eg_wrap, and
mode1.size_out_of_range_320x240).

To do:

- Keep what a job downloads in the Actions cache, as the Pico SDK and
  picotool already are. For apt that is the folder of package files: on
  a hit apt finds every file there and fetches nothing, and on a miss it
  fetches as it does now and the files are saved. The key is the runner
  image and the list of packages. Do it for every job that installs with
  apt, with one key where jobs install the same packages, and for
  anything else a job fetches: the Arm toolchain, Quartus, and whatever
  the other platforms' jobs pull.
- Split the tests by what they cost and not by every other one, so the
  long ones are shared out.

## 2. Video: the scanline table's options, and one table in the fill cache

Two changes to what a fill renderer is handed, done and measured
together.

**The options in the scanline table.** A row of the scanline table says
what draws each plane: today a renderer and a config pointer. It gets the
mode and the options word of the XREG call beside them, so a row says
what was asked for and not only which function draws it.

- `vga_prog_fill` and `vga_prog_exclusive` take the mode and the options
  in place of a renderer. A machine that draws in software looks the
  renderer up as it books the rows. The Pocket writes the mode and the
  options to its FPGA table, as it does now.
- `vga_mode_begin` goes. It exists only to tell the Pocket the mode and
  options that the booking does not carry.
- A renderer is handed the row's options in place of the plane number,
  which only mode 2 reads, and only to find its options. Mode 2's own
  array, `mode2_options`, goes: 3 KB.
- A savestate writes the mode and options from the row. The functions
  that work them out from a renderer go: `vga_mode_fill_id`,
  `mode0_fill_attr`, `mode1_fill_attr`, `mode2_fill_attr`,
  `mode3_fill_attr`, and `mode2_set_options`.
- The renderers that return NULL in a fabric build are no longer asked
  for there.

A row grows from 44 bytes to 52 on the Pico, 4 KB over 512 rows, against
the 3 KB array that goes, so this half costs the Pico 1 KB and saves the
Pocket 3 KB. The union below pays for it six times over. The sprites
keep their renderer alone.

**One table in the fill cache.** A plane's fill cache has two tables: the
256 pixel pairs of 4 bpp, 1 KB, which modes 2 and 3 draw from, and the
pairs of a row of text, 1.3 KB, which modes 0 and 1 draw from. A scanline
of a plane is drawn by one mode, so it uses one of them. They become a
union, with one key that says which is built and from what:

    typedef enum {
        VGA_FILL_NONE,          /* nothing built; a zeroed cache */
        VGA_FILL_MODE0_ROW,
        VGA_FILL_MODE1_4BPPR,   /* mode 1's depths, a row of each */
        VGA_FILL_MODE1_4BPP,
        VGA_FILL_MODE1_8BPP,
        VGA_FILL_MODE1_16BPP,
        VGA_FILL_4BPP,          /* the pairs of 4 bpp */
        VGA_FILL_4BPP_REVERSE,
    } vga_fill_kind_t;

    typedef struct {
        uint32_t key[9];        /* key[0] is the kind */
        union {
            uint32_t pairs_4bpp[256];
            uint32_t row_pair[VGA_FILL_ROW_CELLS][4];
        };
    } vga_fill_cache_t;

A row's key is what it is now, seven words with the kind first. The 4 bpp
key is the kind and the 16 palette entries, eight words, which replaces
`palette_4bpp` and `reverse_4bpp`. Each renderer compares the whole key
and builds on a difference, so a table left by another mode is never
drawn from. The kinds replace `MODE0_ROW_KEY` and mode 1's depth numbers,
which are the same numbers kept in two files today.

A cache goes from 2,384 bytes to 1,332. The Pico has six, a set for each
core, so 6.3 KB; an emulator has three.

What it costs: a plane that changes between a text mode and a 4 bpp mode
down the screen builds a table again at each change, once on each core.
A 4 bpp build is about a scanline's worth of pixels. A program already
pays that for a 4 bpp palette that changes down the screen. Measure a
plane split between mode 1 and mode 3 at 4 bpp, and one that alternates
them every scanline, for late scanlines on the Pico.

`vga_fill_cache_forget_row`, which the emulators call before every
scanline, forgets only a row, so the 4 bpp pairs are not built again
every scanline.

Test: the mode tests on every machine, the HIL `term sizes bus`, mode 2's
speed on the board, and the two split-plane measurements.

## 3. One scratch buffer, in core

Every machine has one 1 KB buffer that the ROM reader fills a record at a
time, but each brings its own: the RIA passes the monitor's `mbuf`, and the
emulators and the Pocket each have a `static buf` with no name inside their
loader. Core gets the one buffer, `buf1k`, in core/sys/buf1k.c, and every
machine links it. The ROM reader reads into it and its functions lose their
buffer argument, the two unnamed buffers go, and the RIA's `mbuf` becomes
`buf1k`. What the RIA's monitor keeps beside it stays in the RIA: how many
bytes it holds, and reading it from the console. No memory changes.

Staying: the file pool's 2.2 KB, whose names reopen the files after a
savestate.

## 4. The line editor's history

The history is 2 KB, 8 lines of 256 bytes, inside the line editor, and
only the monitor uses it. A flag turns it off for every other read, so a
machine without a monitor, the Pocket, still links it.

The history becomes its caller's. The line editor defines what a history
is, the caller says how big its own is, and a read is handed one or
none. The monitor has the one
history and hands it to its command prompt. Its confirmations, the
uploads, and a program's reads hand none, so `rln_read_line_no_history`
and the flag go, and one read function is left with the timeout the
script read has. The history leaves the line editor's savestate chunk,
and the monitor saves its own.

## 5. 64-bit divides

The four library routines are 3.5 KB together, and one goes when nothing
calls it. The callers: the PCM file open, the HID analog scaling, the time
of day, the clock attribute, the APF slot task and the system walk. libc's
`gmtime_r` calls the two signed ones, so those 1.8 KB wait for section 6.

## 6. Our own time library

On every platform, with only what we use: no `strftime`, `tzset` (and the
`sscanf` it pulls in), `mktime`, `localtime`, `gmtime` or `setenv`; 32-bit
day arithmetic. About 6 KB on the Pocket, not measured again, and the last
1.8 KB of the 64-bit divides.
