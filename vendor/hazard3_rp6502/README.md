# Hazard3 overrides

These are modified copies of Hazard3 files. The Pocket's soft CPU builds
them instead of the upstream files of the same name. They will not go
upstream, so code comments stay short and this file holds the reasons.

Upstream base: `vendor/hazard3` at v1.1.1,
`8af992930f71a69b0e06c38734c1094f41a05ca0`.

## Build

- `src/core/rtl.cmake` lists these four files in `RP6502_RTL_SOURCES`.
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
behind the stall signals that pick how far the buffer moves. These copies
take the stall signals off that path without changing what the core does
on any clock.

| Copy | Change |
|---|---|
| hazard3_regfile_1w2r.v | The register file: see below. |
| hazard3_decode.v | A new output, `df_cir_use_go`: how far the fetch buffer moves if X does not stall. `df_cir_use` is the same logic as upstream, built from it. |
| hazard3_frontend.v | A new input, `cir_use_go`. A second copy of the buffer's shift and overlay, driven by it, gives `next_instr_go`, and the register-number predecode reads that. The decompressor still reads the real `next_instr`. |
| hazard3_core.v | The bypass select compares the held register number and the new predecode each against 0, `xm_rd`, `d_rd` and `mw_rd`, and the stall signals choose among the results last. The register file's read address is chosen the same way. `d_rs*_predecoded_nxt` and `mw_rd_nxt` are gone. |

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

### The bypass select

Whenever the stall that holds the decode stage is low, `cir_use` equals
`cir_use_go`, so the predecode sees the same instruction as upstream, and
the rest only reorders the same comparisons.

## How the copies were checked

A lockstep check ran upstream and these copies side by side on the same
inputs and compared every output and the registers the change touches on
every clock: the Pocket's own tests, 82 million clocks, and 65 of
Hazard3's own `test/sim/sw_testcases`. No clock differed, and a fault
planted on purpose stopped the check within 30 clocks. The other ten
upstream cases need an assembler that knows Zcmp.
