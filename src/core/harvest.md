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

## 2. Our own time library

The firmware keeps time with the C library today: newlib on the Pico,
picolibc on the Pocket, and the host's on the emulators. Each machine has
its own copy of the glue, and the three do not agree: the Pico's times
are in the language of `SET LOC`, an emulator's in the host's, and the
Pocket's in English. The library is also what costs the memory. Measured
in the firmware, what goes when nothing calls it:

| | RP6502-PICO (RIA W) | RP6502-FPGA |
|---|---|---|
| `strftime` | 3,350 | 2,500 |
| `tzset` and its limits | 1,550 | 1,540 |
| `sscanf`, which only `tzset` calls | 6,340 | 70 |
| `mktime`, `localtime_r`, `gmtime_r` | 2,500 | 2,020 |
| `setenv` | 500 | 470 |
| `malloc`, `realloc`, `free` | 4,200 | 520 |
| the time locale's strings | 250 | 250 |
| the signed 64-bit divides | | 1,830 |
| **Bytes** | **18,700** | **9,200** |

Less what replaces them. `sscanf`, `setenv`, and `tzset` are the only
callers of `malloc` in the firmware but one: the SDK's Bluetooth setup,
`cyw43_btbus_init`, takes one buffer on an RP6502-RIA W. The RP6502-RIA
and the RP6502-FPGA lose the allocator whole. Two linker wraps on the
Pico that exist to keep `tzset` small, `iswspace` and `sniprintf`, go.

### What it is

One library in core, core/tim, with nothing of the C library's under it
but the types in `<stdint.h>`. Every machine links the same code. A
machine gives it three things:

- **The clock**: the seconds now, and whether a program may set them.
  As today.
- **The zone**: for a time in UTC, the offset from it in seconds,
  whether that is summer time, and the zone's short name.
- **The words**: the names of the days and months, long and short, AM
  and PM, and the four formats `%c %x %X %r` stand for.

Who gives what:

| | The zone | The words |
|---|---|---|
| RP6502-PICO | `SET TZ`: a name from its table, or a POSIX TZ string, read by core/tim's own parser | `SET LOC`'s language, the `STR_TIME_` strings it has now |
| RP6502-FPGA | the Pocket's offset in minutes, as it is; no string is made of it | the `STR_TIME_` strings, in the language it is built in |
| Emulators | the host: `localtime_r` and its `tm_gmtoff` and `tm_zone`; on Windows, `GetTimeZoneInformationForYear` | the host's locale: `nl_langinfo_l`; on Windows, `GetLocaleInfoEx` |

The host's answers come through the OS layer as two small functions,
`os_zone` and `os_time_word`, which replace `os_localtime`, `os_gmtime`,
`os_strftime_local`, `os_tm_apply_zone`, `os_locale_reset`, and
`os_locale_free`. Where a host has no locale to ask, the web, the words
are the `STR_TIME_` strings.

### What it has to do

Its own types: seconds are an `int64_t`, and a broken-down time is its
own struct with the nine fields of a `struct tm`, the offset, and the
zone's name. The C library's `struct tm` differs from one library to the
next in whether it has the last two, which is what `os_tm_apply_zone`
patches over today.

1. **Seconds to a date in UTC**, `gmtime`. Days since the epoch to a
   year, month, and day by the usual 400-year arithmetic, in 32 bits. The
   one 64-bit step, seconds into days, is a divide by a constant done in
   16-bit pieces, so no library divide is called.
2. **A date in UTC to seconds**, with fields out of range carried into
   the next, as `mktime` does: month 13 is January of the next year.
3. **Seconds to a local date**, `localtime`: ask for the zone, add the
   offset, and do 1.
4. **A local date to seconds**, `mktime`: do 2, then take off the
   zone's offset at that time, and again if the offset there differs. A
   time that summer time skips or repeats goes by `tm_isdst`, as C says.
5. **The POSIX TZ string**: the standard name and offset, the summer
   name and offset, and the two rules, in the `Mm.w.d/time` form the
   table uses and the `Jn` and `n` forms a typed string may. Names in
   angle brackets. Parsed once when the setting changes, into a small
   struct, with no `sscanf`.
6. **`strftime`**: every conversion of C99, which is what the manual
   promises: `%a %A %b %B %c %C %d %D %e %F %g %G %h %H %I %j %m %M %n %p
   %r %R %S %t %T %u %U %V %w %W %x %X %y %Y %z %Z %%`. `%E` and `%O`
   have no effect, as now. `%z` and `%Z` come from the zone and
   `tm_isdst`, as now, because the 6502's `struct tm` has no offset. The
   words go through the code page as they are written.

### Who calls it

- core/api/clk.c: the 6502's `GMTIME`, `LOCALTIME`, `MKTIME`, and
  `STRFTIME`. It calls the C library's `mktime` itself today.
- The three copies of the glue become one: core/api/tim.c on the
  emulators, ria/api/tim.c, and pocket/sw/time.c keep only their clock
  and their zone.
- The Pico's `STATUS` line, and the time a file is stamped with on a
  USB drive (ria/usb/msc.c) and on the Pocket's card.
- osal/posix/dir.c sets a host file's time with the host's `mktime`.
  That is the host's own file API and stays.

### How it is tested

On a host, against the host's C library: for every zone in the Pico's
table and a spread of times across 1900 to 2100, with the hour either
side of each change of summer time, `localtime`, `mktime`, and each
conversion of `strftime` in the C locale must give what glibc gives with
the same TZ string. The same tests run on every machine's build, and the
RTL tests run them on the Pocket's firmware.

### Order

1. core/tim with its tests, linked nowhere else.
2. The emulators on it, with `os_zone` and `os_time_word`.
3. The RP6502-FPGA on it. Measure.
4. The RP6502-PICO on it; the wraps and `STR_TZ` go. Measure.
5. The manual: the RP6502-FPGA and the web no longer answer in the C
   locale.

### Beside it

`HELP SET` on the RP6502-PICO does not list `SET LOC`. It is missing
from `STR_HELP_SET` in every language.

### To decide

- Whether `strftime` takes more than C99's conversions. newlib also
  has `%k %l %P %s`, and a program may use them today on the Pico.
- Whether an RP6502-RIA W keeps the allocator for the one Bluetooth
  buffer, or the SDK's override gives it a buffer of its own.

## 3. 64-bit divides

On the RP6502-FPGA the four library routines are 3.5 KB together, and
one goes when nothing calls it. Its callers there:

| | Routine |
|---|---|
| `apf_slot_task`, four places | signed divide |
| `hid_scale_analog` | signed divide |
| `gmtime_r`, the C library's | signed divide and remainder |
| `tim_get_time` | unsigned divide and remainder |
| `attr_get_clk_run` | unsigned divide |
| `pcm_open_fmt` | unsigned divide |
| `sys_walk` | unsigned divide |

`gmtime_r` goes with section 2, so what this section saved is measured
after it.

The RP6502-PICO keeps its one routine, `__udivmoddi4`, 830 bytes. The
SDK calls it: `printf` of a 64-bit number, the Wi-Fi driver, lwIP's
clock, and TinyUSB's. Our own callers there are the same ones, with the
MIDI and mass storage drivers and the UF2 progress line, and rewriting
them saves nothing on the Pico.

Each caller is looked at for what it divides. Most divide a count of
microseconds by a constant or by a 32-bit number, and a time in
microseconds that is kept as seconds and a remainder never needs the
divide at all.
