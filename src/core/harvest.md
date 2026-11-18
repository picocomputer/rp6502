# Harvest memory: work still to do

Memory that comes back on every machine, to be done on its own branch.
No machine is short of memory now; the Pocket's soft CPU has its RAM in
SDRAM. This work keeps every machine lean and its code plain.

## 1. Options in the scanline table

Mode 2 keeps the options word of its XREG call in an array of its own, for
every scanline and plane: 3 KB that the Pocket never reads, since its FPGA
table already holds the options. A row of the scanline table gets an
options word for each plane, beside the renderer and the config pointer.
Mode 2 reads it from the row and `mode2_options` goes. A savestate reads it
from the row too, so the functions in modes 0, 1 and 3 that work the
options out from the renderer go. 3 KB on the Pocket, about 1 KB on the
Pico. Check mode 2's speed on the board.

## 2. 64-bit divides

The four library routines are 3.5 KB together, and one goes when nothing
calls it. The callers: the PCM file open, the HID analog scaling, the time
of day, the clock attribute, the APF slot task and the system walk. libc's
`gmtime_r` calls the two signed ones, so those 1.8 KB wait for section 5.

## 3. The line editor's history

2 KB, 8 lines of 256 bytes, and serves only the monitor, which the Pocket
doesn't have. A flag turns it off as the program runs, so the linker keeps
it. The monitor brings the history, and a machine without a monitor
doesn't link it.

## 4. One scratch buffer, in core

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

## 5. Our own time library

On every platform, with only what we use: no `strftime`, `tzset` (and the
`sscanf` it pulls in), `mktime`, `localtime`, `gmtime` or `setenv`; 32-bit
day arithmetic. About 6 KB on the Pocket, not measured again, and the last
1.8 KB of the 64-bit divides.

## 6. The row color tables of modes 0 and 1

They are 1.3 KB for each plane on each core, 8 KB in all, beside a 1 KB
table for 4 bpp. A plane draws one or the other on a scanline, so the two
can share their memory, and the 8 KB becomes about 1.6 KB. Not tried.

## 7. The cells of one console at a time

The 40-column and the 80-column console both follow every byte, and each
has an alternate screen, so the cells and their colors are paid four
times: 30.7 KB of cells and 46.1 KB of colors on the Pico. Keep the cells
of only one of the two at a time and rebuild the other when the canvas
changes. That halves the cells, if what the rebuild loses is acceptable.
It also halves what a scroll between left and right margins and the
rectangle sequences cost, since both consoles do them today.
