# Harvest memory: work still to do

Memory that comes back on every machine, to be done on its own branch.
No machine is short of memory now; the Pocket's soft CPU has its RAM in
SDRAM. This work keeps every machine lean and its code plain.

Each section is done properly: where the architecture is what keeps the
memory, the architecture changes, and no section is a link-time trick.

## 1. The RTL tests on CI

Not memory, but waiting on it holds up every branch. CI runs the RTL
tests as two jobs, A and B, each on its own runner, and `ctest -I` gives
A the odd tests and B the even ones. On 2026-11-18 A took 36 minutes and
B took 14. Two things were wrong, and both are changed and wait for CI
to show it:

- A's runner spent 18 minutes fetching packages from a mirror that
  stalled. Every job that installs with apt now keeps the package files
  in the Actions cache.
- Both jobs spent 8 minutes building. Half of that was link-time
  optimization, which compiled the simulation again for each of 79 test
  programs. The simulation tree no longer uses it: a build is 90 seconds
  on four cores, and the tests run 2.5% slower.

Left, if a run still shows it worth doing: A's half of the tests costs
22% more than B's, because every other test gives A the three longest.
Splitting them by what they cost is worth about a minute.

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
