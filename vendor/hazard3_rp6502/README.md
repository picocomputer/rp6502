# Hazard3 overrides

These are modified copies of Hazard3 files. The Pocket's soft CPU builds
them instead of the upstream files of the same name. They will not go
upstream, so code comments stay short and this file holds the reasons.

Upstream base: `vendor/hazard3` at v1.1.1,
`8af992930f71a69b0e06c38734c1094f41a05ca0`.

## Build

- `src/core/rtl.cmake` lists these five files in `RP6502_RTL_SOURCES`.
  Every other Hazard3 module is found by module name in the upstream
  `hdl` folders (`-y` for Verilator, `SEARCH_PATH` for Quartus), so a
  module named here is taken from here and never from upstream.
- The copies keep upstream's license headers and formatting, so
  `diff -u` against upstream shows only real changes. Each change is
  marked `rp6502:` in the code.

## Why

The soft CPU runs at 50.4 MHz on the machine's clock in a Cyclone V
5CEBA4F23C8. Hazard3 is written with ASICs in mind, and on this FPGA its
longest path was the bypass select: the next instruction's register
numbers decoded through the fetch buffer and the compressed expander,
behind the stall signals that pick how far the buffer moves. After that
it was the same buffer move feeding the compressed expander into CIR.
These copies take the stall signals off both paths without changing what
the core does on any clock.

| Copy | Change |
|---|---|
| hazard3_regfile_1w2r.v | The register file: see below. |
| hazard3_decode.v | A new output, `df_cir_use_go`: how far the fetch buffer moves if X does not stall. `df_cir_use` is the same logic as upstream, built from it. |
| hazard3_frontend.v | A new input, `cir_use_go`. The buffer's next state is built twice, for `cir_use` = 0 and for `cir_use` = `cir_use_go`, with an expander on each, and `cir_use` picks one last. The register-number predecode reads the second. The uop counter lives here. |
| hazard3_instr_decompress.v | The uop counter moved out to the frontend, so two copies can share it: the step comes in and its next value goes out. No clock or reset. |
| hazard3_core.v | Wires `df_cir_use_go` from decode to the frontend. |

### The register file

`hazard3_regfile_1w2r` is the CPU's 32 registers: one write port and two
read ports ("1w2r"), because an instruction reads up to two registers and
writes one. Upstream tells Yosys on iCE40 and Vivado on Xilinx how to
build it, and gives Quartus no hint. Without one, Quartus has to make a
read of the register written in the same clock return the new value,
which costs logic in front of the memory, or it builds the array out of
flops.

The copy adds one attribute, `ramstyle = "no_rw_check, MLAB"`: build it
in the small LUT memories (MLABs) and skip that read-during-write logic.
That is safe because the core never uses the memory's answer for such a
read: it takes the value from its bypass, `xm_result` or `mw_result`, or
zero for x0. Quartus then builds it as two copies, one for each read
port, in 4 MLABs, 40 ALMs, and no M10K block.

### The fetch buffer and CIR

Upstream moves the fetch buffer by `cir_use`, overlays fetched data,
expands the result and loads it into CIR. `cir_use` waits for `x_stall`,
which comes late, so the whole chain waited for it.

`cir_use` is only ever 0 (X stalls, or D takes nothing) or `cir_use_go`.
The frontend builds the buffer's next contents, its level, its room for
fetched data and the expanded instruction once for each, from signals that
do not wait for `x_stall`. `cir_use` picks between them just before the
registers. That takes a second expander, about 110 ALMs, and drops
upstream's shift by `cir_use`. With Zcmp the expander has a uop counter,
so it moved to the frontend, where both copies read it and the chosen one
sets its next value.

The register-number predecode reads only the `cir_use_go` copy, so it
no longer waits for `x_stall` either. The core uses it only when D does
not hold its instruction, and then `cir_use` equals `cir_use_go`, so it
matches upstream wherever it is used.

Fetched data from the bus (`hrdata`) still runs through the overlay and
the expander into CIR, as it did upstream.

## How the copies were checked

A lockstep check ran upstream and these copies side by side on the same
inputs and compared every output and the registers the change touches on
every clock, CIR, the fetch buffer, the FIFO and the uop counter among
them: the Pocket's own tests (pocket, psleep, pfile and riscv), 128
million clocks, and 65 of Hazard3's own `test/sim/sw_testcases` in their
all-features build, Zcmp included. No clock differed, and a fault planted
on purpose stopped the check within 250 clocks. The other ten upstream
cases need an assembler that knows Zcmp's register moves.
