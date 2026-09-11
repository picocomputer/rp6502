# Trimming and rewriting the code comments

Most of the comments in this tree were written by an AI and most of them should
not exist. Stages 1 through 6 are done. This file describes the rules and what
is left, so the work can be picked up without re-deriving any of it.

## The test a comment has to pass

A comment has to save the reader from reading the code. If the reader ends up
reading the code anyway, the comment cost them time and returned nothing.

Keep a comment only when it gives one of these:

- a measurement, a timing number, a benchmark
- a hardware quirk, an errata, a datasheet fact
- an ordering constraint, a race, or why something is safe
- the derivation of a magic number

If it takes thought to decide whether a comment earns its place, it does not.

Delete:

- anything that restates the code
- anything whose reason is obvious from the surrounding names
- section-divider banners
- a boilerplate line above a macro that only says what the macro is
- anything describing what the code used to do, what was tried, or what turned
  out to be wrong. Working code that is understood does not need its history.
- commentary about work in progress

## The voice for what survives

Modelled on the Sphinx docs in the picocomputer.github.io repository. Full
sentences with explicit subjects. The reason joined to the fact with because,
since or so. Terms defined at first use. Present tense. Plain word order.

Not this:

- a sentence with its subject dropped, such as "Words, not bytes."
- a metaphor used as a mechanism's name and then as a grammatical subject
- a reason given its own sentence, shaped like a principle
- inverted word order, such as "What a drive is called is the machine's to say"
- an aside to the reader

Never invent an actor to give a sentence a subject. Much of this code is
written in the passive because the code does not show which side acts.

The em dash is not a marker of AI writing and is not being removed. Neither
is a trailing "which is" clause: "which is why you don't see any in this
scripting language" and "which is how they detect screen size" are both the
author's, so do not flag that shape.

### Do not give hardware a personality

A part does not want, ask, know, hear, see, say, owe, hand something back or
lend anything. Say what it does. A register is read, a reply is sent, a
message is printed, a device is present or absent.

The exception is a request that is genuinely a request. A program asking for
the mouse and a script asking for frames are both real, because a program and
a script issue commands.

| was | now |
|---|---|
| the drive answers to `FS:` | `FS:` is a name you can use for the drive |
| a frontend with no cursor to lend | a frontend that provides no cursor |
| the core says as much on screen | the core prints a message |
| the terminal answers the queries | the terminal replies to the queries |

### Ownership is a borrowing term, not a permanent home

In this codebase ownership means something that can be passed and borrowed,
the way a lock or a buffer is owned. Do not use it for where a thing lives or
which part it belongs with.

| was | now |
|---|---|
| paths are the host's | paths use the host's format |
| a script's frames are its own | a script sets its own frame count |
| the screen does not belong in the file | the screen would end up in the file |
| the RTL belongs to the FPGA | the RTL is in the FPGA |
| a machine that owns a real clock | a machine that has a real clock |
| every command here belongs to a pico | every command here applies only to a pico |

"Its own" meaning separate or dedicated is ordinary English and stays. "Each
character can have its own foreground" and "the 6502's own vectors" are fine.

### Say what a name is, not how it is spelled

"Spelling" was used for a name, a notation, a byte encoding and a color
format. Each of those has a real word.

| was | now |
|---|---|
| ends a line on either spelling | ends a line on a carriage return or a line feed |
| FatFs's own spelling | FatFs's own notation |
| the machine's own spelling | the machine's own path format |
| its code-page spelling | the code page's bytes rather than UTF-8 |
| other xterm spellings | other xterm color formats |

### Words that are banned as the name of a mechanism

The wire, the walk, the seam, the latch, the ask, the doing, the roster, and
the phrase "so it is told". Where one appears, name the thing the code calls
it. Stages 1 through 3 left these behind and they were cleaned up afterward,
so check for them rather than assuming an earlier stage caught them.

Counts still outstanding in the unfinished stages:

| word | occurrences |
|---|---|
| the wire | 25 |
| the walk | 21 |
| the seam | 20 |
| the latch | 12 |
| the ask | 9 |
| the roster | 7 |
| so it is told | 4 |
| the doing | 2 |

The three patterns above are unswept. Across `src` and `tests` there are 89
uses of "owns", 41 of "belongs to", 53 of "spelling" and 8 of "answers to".
Most will be legitimate. Read each one.

"The fabric" appears 90 times and needs judgment rather than a sweep. Fabric is
the ordinary word for an FPGA's programmable logic and is fine as a noun. It is
not fine as an actor that renders, paces, sizes or is told things, which is how
seven of those uses read.

The line "This driver's row in a machine's driver list; see core/sys/driver.h."
sits above a `DRIVER` macro in 36 places and says only what the macro below it
says. Delete it, keeping any substantive clause that follows it on the same
line.

## Files where the default flips

Some files are mostly the author's own writing. There, leave a comment alone
unless it is clearly AI-written. Those are `src/core/term/term.c`, `term.h`,
`src/core/str/rln.c`, `rln.h`, and the older one-line comments in
`src/host/pico/ria`.

His writing looks like: numbered invariants, a timeout justified against a real
constraint, a race analysed with its consequence bounded, a direct address to
the reader such as "The logic herein will make more sense if you remember
this", and hand-checked arithmetic bounds. Leave all of it.

The AI comments in those same files look like the rest of this job: dropped
subjects, a metaphor used as a mechanism's name, slogans, reasons written as
principles. Those are in scope.

No file is skipped. Every file has at least a few AI comments in it.

Not touched at all: the four `dist/README.txt` files, `README.md` at the root,
and `src/host/sokol/android/README.md`.

## Method

Four passes per group of files, run unattended.

1. **Write.** Read the code before writing a word about it: the function, its
   callers, the struct it touches, the vendored header it depends on. Every
   claim must be verified against code read in that session. Check whether a
   claim holds on every path, not just the common one. A claim that cannot be
   verified is deleted and reported, never paraphrased.
2. **Review.** Read the code and check every remaining claim against it. Report
   contradictions, invented actors, surviving bad style, and comments that
   should have been deleted.
3. **Fix** what the review found.
4. **Review again**, then fix again, then review a third time.

The reviews are what make this work. A writer alone ships plausible-sounding
claims that the code does not support; two bulk rewrites had to be thrown away
before this shape settled. A comment flagged twice is deleted rather than
corrected a third time.

## Stages

Stages 1 through 6 are done and pushed to the libre8 branch.

| stage | scope | state |
|---|---|---|
| 1 | `src/core/sys`, `wdc`, `ria`, `com` | done |
| 2 | `src/core/api`, `rom`, `str` | done |
| 3 | `src/core/aud`, `vga` | done |
| 4 | `src/core/hid`, `dap`, `term` | done |
| 5 | `src/host/sokol` | done |
| 6 | `src/host/libretro`, `src/host/itch.io`, `src/osal` | done |
| 7 | `src/host/pocket/sw`, `gen`, `quartus` | to do |
| 8 | `src/host/pocket/core` | to do |
| 9 | `src/host/pico` | to do |
| 10 | `tests` | to do |
| 11 | `src/host/pocket/README.md` | to do |

Sizes of what is left:

| stage | files | comment lines |
|---|---|---|
| 7 | 48 | about 2300 |
| 8 | 16 SystemVerilog | about 1900 |
| 9 | 115 | about 4700 |
| 10 | 188 | about 8100 |
| 11 | one file | 905 lines total |

Stage 9 is the biggest risk to voice, because the older one-line comments in
`src/host/pico/ria` are the author's. Stage 10 is the largest by volume and the
easiest, because a test's comments are mostly narration of the assertions
beside them.

## The docs got the same pass

The Sphinx sources in the picocomputer.github.io repository were the model for
the voice, so they were read the same way to check that the difference is still
detectable. They are mostly the author's writing with AI passages mixed in.

Forty-five passages were rewritten across emu.rst, term.rst, ria.rst, os.rst,
index.rst, vga.rst, sdk.rst and fpga.rst. Nothing was found in pico.rst or
ria_w.rst. The concentration was in emu.rst, which is also where the most
recent AI writing went. One measurable tell: before the pass, "rather than"
appeared eight times in emu.rst and not at all in os.rst, index.rst, sdk.rst,
vga.rst or ria_w.rst.

Those edits are uncommitted. The docs are the author's to commit.

## Verification

Every stage builds clean and leaves the suites passing:

```
cmake --build build/sokol -j16
cmake --build build/libretro -j16
ctest --test-dir build/sokol -LE pico     # 118
ctest --test-dir build/libretro           # 84
```

Stages 7, 8 and 11 also need the Pocket firmware and the RTL:

```
ninja -C build/pocket assets/sw.bin
cmake --build build/rtl -j16
ctest --test-dir build/rtl                # 131
```

Stage 9 also needs `cmake --build build/ria-pico2 -j16`.

`ctest` does not build. Build the tree first or the run reports against stale
binaries. Never run the `pico` ctest label, which drives the attached board.

Each stage reports how many comments it deleted against how many it kept. That
number is an outcome of the rules, not a target. The check is whether each
decision followed the rules, which is what the review passes read for.

## What the reviews have been catching

The comments were not only badly written, they were often wrong. These were
found by reading the code the comment described, and they are the reason the
review passes exist:

- `sst.h` promised a rollback that the trusted-blob path skips.
- `mix.h` said the Pico PWM runs at the native rate. It runs at 49718 Hz.
- `rsmp.sv` was off by a factor of 4096 on a fraction.
- `mode1.sv` named the wrong end of a byte.
- `mode4.sv` and `mode5.sv` credited a scaffold that does not exist.
- `hid.h` claimed a zeroed connection struct would read as a real device. Every
  mount hook rejects one on its own valid flag.
- `vtkeys.h` said pasted control bytes are dropped. They pass through.
- `cc65dbg.h` said the C stack pointer holds the frame base. The frame base is
  that pointer plus the frame size.
- `dbg.h` named a scanner that exists nowhere in the tree.
- `dwarf_line.h` called a DWARF path full when it is relative whenever the
  directory entry is.
- `color.c` said the terminal renders from its palette copy. It resolves
  indices when a cell is written, so changing a palette entry does not recolor
  what is already on screen.
- `script.c` said the VIA and the RIA answer the bus above $FEFF. Nothing
  answers $FF00 to $FFCF at all.
- `dbgui.h` claimed nothing else in the file reaches the CPU. The draw function
  reads live CPU state and its step buttons call into the debugger.
- `gamepad.c` derived 250 ms from fifteen frames. Attempts are sixteen frames
  apart, about 267 ms.
- `retro.c` said a fresh start does not inherit the working directory. A chdir
  moves the whole host process and nothing in a reboot puts it back.
- The libretro input code gave one coordinate range for the pointer and the
  lightgun. The gun runs to -0x8000, which means out of bounds.
- The Windows file layer borrowed the POSIX reason for making a path absolute
  after the open succeeds. On Windows the call is lexical and resolves a name
  that does not exist yet, so only half the reason holds.
- `windows.cmake` derived a Windows version floor of 0x0600 and then set
  0x0601, and missed one of the calls sitting behind that gate.
- `os.h` called the monotonic clock real time. Both implementations read a
  monotonic source, and the sibling files treat the two as distinct.
- The POSIX file layer claimed a low ROM descriptor could be reached by a 6502
  program. A program names a descriptor pool row, never an OS descriptor.
- Both `errmap.h` files warned about a header that could shadow the C library's
  `errno.h`. No build puts those directories on an include path.

Two of these turned out to be defects in the code rather than the comment, and
both are fixed. The Windows gamepad hash used an FNV-1a offset basis with its
last two digits missing, so it was not the algorithm it named. Nothing was
broken by it, because the ids never leave the running process and FNV-1a is
well defined for any nonzero basis. The keypad branch inside the sokol ASCII
conversion helper could not be reached, because every keypad digit is handled
by an explicit case that breaks before the label calling it.


Do not write comments about mistakes. A mistake in half-finished code stops
happening once the code is understood, and a comment recording it adds nothing
for the next reader. This section is a note on method, not a template.
