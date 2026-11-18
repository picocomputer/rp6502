# Harvest memory: work still to do

Memory that comes back on every machine, to be done on its own branch.
No machine is short of memory now; the Pocket's soft CPU has its RAM in
SDRAM. This work keeps every machine lean and its code plain.

Each section is done properly: where the architecture is what keeps the
memory, the architecture changes, and no section is a link-time trick.

## 1. The RTL tests on CI

Not memory, but waiting on it holds up every branch. CI runs the RTL
tests as two
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

## 2. One scratch buffer, in core

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

## 3. The line editor's history

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

## 4. 64-bit divides

The four library routines are 3.5 KB together, and one goes when nothing
calls it. The callers: the PCM file open, the HID analog scaling, the time
of day, the clock attribute, the APF slot task and the system walk. libc's
`gmtime_r` calls the two signed ones, so those 1.8 KB wait for section 5.

## 5. Our own time library

On every platform, with only what we use: no `strftime`, `tzset` (and the
`sscanf` it pulls in), `mktime`, `localtime`, `gmtime` or `setenv`; 32-bit
day arithmetic. About 6 KB on the Pocket, not measured again, and the last
1.8 KB of the 64-bit divides.
