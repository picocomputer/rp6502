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
B took 14. Two things were wrong and are changed, and the next run took
14.5 and 12.4:

- A's runner spent 18 minutes fetching packages from a mirror that
  stalled. Every job that installs with apt now keeps the package files
  in the Actions cache, and the step is under a minute.
- Both jobs spent 8 minutes building. Half of that was link-time
  optimization, which compiled the simulation again for each of 79 test
  programs. The simulation tree no longer uses it, and the build is 4
  minutes. On the bench the tests run 2.5% slower without it. On CI one
  job's tests took the same time and the other's 28% longer, which is
  what one runner differs from the next by.

Left: the tests are 6 to 9 minutes of each job, and the runners differ
by half again. A's half costs 22% more than B's, because every other
test gives A the three longest. Splitting them by what they cost, or
into three jobs, is what would shorten it further.

## 2. 64-bit divides

The four library routines are 3.5 KB together, and one goes when nothing
calls it. The callers: the PCM file open, the HID analog scaling, the time
of day, the clock attribute, the APF slot task and the system walk. libc's
`gmtime_r` calls the two signed ones, so those 1.8 KB wait for section 3.

## 3. Our own time library

On every platform, with only what we use: no `strftime`, `tzset` (and the
`sscanf` it pulls in), `mktime`, `localtime`, `gmtime` or `setenv`; 32-bit
day arithmetic. About 6 KB on the Pocket, not measured again, and the last
1.8 KB of the 64-bit divides.
