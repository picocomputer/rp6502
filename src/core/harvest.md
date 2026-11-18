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

On the RP6502-FPGA the library's three routines left are 2.6 KB
together, and one goes when nothing calls it. The time library took the
fourth with it, the signed remainder, 900 bytes. Its callers there:

| | Routine |
|---|---|
| `apf_slot_task`, four places | signed divide |
| `hid_scale_analog` | signed divide |
| `tim_get_time` | unsigned divide and remainder |
| `attr_get_clk_run` | unsigned divide |
| `pcm_open_fmt` | unsigned divide |
| `sys_walk` | unsigned divide |

The RP6502-PICO keeps its one routine, `__udivmoddi4`, 830 bytes. The
SDK calls it: `printf` of a 64-bit number, the Wi-Fi driver, lwIP's
clock, and TinyUSB's. Our own callers there are the same ones, with the
MIDI and mass storage drivers and the UF2 progress line, and rewriting
them saves nothing on the Pico.

Each caller is looked at for what it divides. Most divide a count of
microseconds by a constant or by a 32-bit number, and a time in
microseconds that is kept as seconds and a remainder never needs the
divide at all. core/tim has a divide by a number under 65536 in 32-bit
steps, `tim_divide`, which may serve the others.
