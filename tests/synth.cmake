# Synthesis: the same RTL through Quartus, for area, timing and a bitstream.
# Not a test, but it needs the source lists rp6502_verilate.cmake builds, so
# it is included from tests/ rather than duplicating them in src/fpga.

# project into the build tree and runs the fitter and the analyzer. The
# source list is the verilated one, so the thing measured is the thing
# tested, and the generated packages come from this build rather than a
# copy that can drift.
find_program(QUARTUS_MAP quartus_map HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
find_program(QUARTUS_FIT quartus_fit HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
find_program(QUARTUS_STA quartus_sta HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
if(QUARTUS_MAP AND QUARTUS_FIT AND QUARTUS_STA)
    set(SYNTH_DIR ${CMAKE_CURRENT_BINARY_DIR}/synth)
    set(SYNTH_QSF ${SYNTH_DIR}/rp6502.qsf)
    set(SYNTH_SDC ${RP6502_SRC}/fpga/platform/pocket/quartus/rp6502.sdc)
    set(SYNTH_LINES
        "set_global_assignment -name FAMILY \"Cyclone V\""
        "set_global_assignment -name DEVICE 5CEBA4F23C8"
        "set_global_assignment -name TOP_LEVEL_ENTITY rp6502"
        "set_global_assignment -name PROJECT_OUTPUT_DIRECTORY output_files"
        "set_global_assignment -name SDC_FILE ${SYNTH_SDC}"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl/arith"
        # A shift register the fitter recognises becomes an M10K, and the
        # ones here are short enough that a block is a poor trade: three
        # blocks go back for a handful of ALMs. Blocks are what this
        # device is short of, not logic.
        "set_global_assignment -name AUTO_SHIFT_REGISTER_RECOGNITION OFF"
        # The machine's own ports outnumber the package's pins — the host
        # window alone is a hundred of them — and this target exists to
        # measure area, not to be bound to pads.
        "set_instance_assignment -name VIRTUAL_PIN ON -to *"
        # -to * means Quartus reports every node it declined to make a
        # virtual pin, which is ten thousand messages — its cap — and
        # buries the hundred and sixty that mean something. Only this
        # project has the assignment, so only this project hides it.
        "set_global_assignment -name MESSAGE_DISABLE 15720")
    foreach(src ${RP6502_MACHINE_SOURCES})
        if(src MATCHES "\\.sv$")
            list(APPEND SYNTH_LINES
                "set_global_assignment -name SYSTEMVERILOG_FILE ${src}")
        elseif(src MATCHES "\\.v$")
            list(APPEND SYNTH_LINES
                "set_global_assignment -name VERILOG_FILE ${src}")
        endif()
    endforeach()
    string(REPLACE ";" "\n" SYNTH_QSF_TEXT "${SYNTH_LINES}")
    file(MAKE_DIRECTORY ${SYNTH_DIR})
    file(WRITE ${SYNTH_QSF} "${SYNTH_QSF_TEXT}\n")
    file(WRITE ${SYNTH_DIR}/rp6502.qpf
        "PROJECT_REVISION = \"rp6502\"\n")
    add_custom_target(synth
        COMMAND ${QUARTUS_MAP} rp6502
        COMMAND ${QUARTUS_FIT} rp6502
        COMMAND ${QUARTUS_STA} rp6502
        WORKING_DIRECTORY ${SYNTH_DIR}
        DEPENDS cpu65_rom vid_font_rom vid_palette_rom aud_sine_rom
        COMMENT "Synthesizing the machine for the Pocket's Cyclone V"
        VERBATIM)

    # And the same again with the Pocket's own layer on top, which is
    # what actually has to fit: the machine alone leaves out the bridge,
    # the SDRAM controller, the I2S and the video crossing. The ports
    # are virtual because pocket_core presents more signals than the
    # package has pins — Analogue's core_top is what binds them to pads,
    # and it is not vendored yet.
    set(PSYNTH_DIR ${CMAKE_CURRENT_BINARY_DIR}/synth_pocket)
    set(PSYNTH_QSF ${PSYNTH_DIR}/pocket.qsf)
    set(PSYNTH_LINES
        "set_global_assignment -name FAMILY \"Cyclone V\""
        "set_global_assignment -name DEVICE 5CEBA4F23C8"
        "set_global_assignment -name TOP_LEVEL_ENTITY pocket_core"
        "set_global_assignment -name PROJECT_OUTPUT_DIRECTORY output_files"
        "set_global_assignment -name SDC_FILE ${SYNTH_SDC}"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl/arith"
        "set_instance_assignment -name VIRTUAL_PIN ON -to *")
    foreach(src ${RP6502_MACHINE_SOURCES})
        if(src MATCHES "\\.sv$")
            list(APPEND PSYNTH_LINES
                "set_global_assignment -name SYSTEMVERILOG_FILE ${src}")
        elseif(src MATCHES "\\.v$")
            list(APPEND PSYNTH_LINES
                "set_global_assignment -name VERILOG_FILE ${src}")
        endif()
    endforeach()
    foreach(src pocket_fifo pocket_video pocket_i2s pocket_sdram pocket_sram
            pocket_bridge pocket_file pocket_core)
        list(APPEND PSYNTH_LINES
            "set_global_assignment -name SYSTEMVERILOG_FILE ${RP6502_FPGA_POCKET}/${src}.sv")
    endforeach()
    string(REPLACE ";" "\n" PSYNTH_QSF_TEXT "${PSYNTH_LINES}")
    file(MAKE_DIRECTORY ${PSYNTH_DIR})
    file(WRITE ${PSYNTH_QSF} "${PSYNTH_QSF_TEXT}\n")
    file(WRITE ${PSYNTH_DIR}/pocket.qpf "PROJECT_REVISION = \"pocket\"\n")
    # The real thing: Analogue's framework on top, their pin assignments,
    # their apf_top as the root, and a bitstream at the end of it. The
    # pins and device come from the vendored project verbatim — only its
    # own source list is dropped, because those files are what we
    # replace.
    set(BS_DIR ${CMAKE_CURRENT_BINARY_DIR}/bitstream)
    set(BS_QSF ${BS_DIR}/rp6502.qsf)
    set(APF ${RP6502_VENDOR}/openfpga/src/fpga)
    # Analogue's framework is a submodule the simulation never needs, so
    # it is often absent — CI checks out only what it builds.
    # The array the firmware is loaded into is sized in the RTL, so read
    # the size from there rather than keeping a second copy in step.
    file(STRINGS ${RP6502_SRC}/fpga/rtl/rv/rv_soc.sv TCM_WORDS_LINE
        REGEX "localparam int TCM_WORDS")
    string(REGEX MATCH "[0-9]+" TCM_WORDS "${TCM_WORDS_LINE}")

    # No firmware, no bitstream. A bitstream without it boots to a soft
    # CPU fetching zeros, which looks like dead hardware and is not.
    if(EXISTS ${APF}/ap_core.qsf AND SW_BIN)
    file(MAKE_DIRECTORY ${BS_DIR})
    # apf_constraints.sdc reads core/core_constraints.sdc relative to the
    # project directory — the framework's hook for a core's own groups.
    # Stage it where that read looks, instead of listing it a second time
    # and letting the vendor's read fail as a Critical Warning every run.
    # Ours, not the template's: vendor/openfpga's copy groups a PLL this
    # core does not instantiate, so its four filters matched nothing.
    file(COPY ${RP6502_SRC}/fpga/platform/pocket/quartus/core_constraints.sdc
        DESTINATION ${BS_DIR}/core)
    file(STRINGS ${APF}/ap_core.qsf BS_TEMPLATE)
    set(BS_LINES "")
    foreach(line ${BS_TEMPLATE})
        if(NOT line MATCHES "_FILE|SEARCH_PATH|SIGNALTAP")
            list(APPEND BS_LINES "${line}")
        endif()
    endforeach()
    list(APPEND BS_LINES
        "set_global_assignment -name GENERATE_RBF_FILE ON"
        # Analogue's template asks for Auto Fit, which stops optimizing
        # the moment timing is met. That is why hold on the soft CPU's
        # crossing into the machine landed at a single picosecond: met,
        # and abandoned there. Standard Fit keeps going, and the margin
        # it finds is what survives the next fit's placement.
        "set_global_assignment -name FITTER_EFFORT \"STANDARD FIT\""
        # Name the synchronisers. Quartus finds all 78 chains on its own
        # and then discards them — "the design MTBF is not calculated
        # because there are no specified synchronizers" — so every
        # crossing into a chain's own first stage was reported as
        # unsynchronised, and the design had no reliability figure at
        # all. IF ASYNCHRONOUS rather than FORCED because the clk_sys and
        # clk_rv pair is deliberately one synchronous group: forcing
        # would invent synchronisers across a seam that is not a
        # crossing.
        "set_global_assignment -name SYNCHRONIZER_IDENTIFICATION \"FORCED IF ASYNCHRONOUS\""
        # Put the SRAM's launch and capture flops in the pads. In fabric
        # they measure about 8.2 ns each way on this part; in the I/O
        # element they are around 3. The interface has 24.4 ns for both
        # crossings once tAA has taken its 55 out of the 6502's four
        # clocks, so fabric spends two thirds of the budget on routing
        # and the IOE spends a quarter. It is also the only way the
        # placement stays put between fits.
        )
    foreach(pin sram_a sram_dq sram_oe_n sram_we_n sram_ub_n sram_lb_n)
        list(APPEND BS_LINES
            "set_instance_assignment -name FAST_OUTPUT_REGISTER ON -to ${pin}")
    endforeach()
    # The SDRAM's read return is the tight direction and always was: the
    # memory clock is forwarded half a period late so the chip has time
    # to sample what we launch, which charges the return path for it.
    # The chip puts data out tAC 6.0 ns after its own edge and we capture
    # on the next clk_sys, so the whole pad crossing has 3.92 ns. In
    # fabric that crossing measures over eight; in the I/O element it is
    # under two.
    foreach(pin dram_a dram_ba dram_dq dram_dqm dram_cke
            dram_ras_n dram_cas_n dram_we_n)
        list(APPEND BS_LINES
            "set_instance_assignment -name FAST_OUTPUT_REGISTER ON -to ${pin}")
    endforeach()
    list(APPEND BS_LINES
        "set_instance_assignment -name FAST_INPUT_REGISTER ON -to dram_dq"
        "set_instance_assignment -name FAST_INPUT_REGISTER ON -to sram_dq"
        "set_global_assignment -name SEARCH_PATH ${BS_DIR}"
        "set_global_assignment -name QIP_FILE ${APF}/apf/apf.qip"
        "set_global_assignment -name VERILOG_FILE ${RP6502_VENDOR}/openfpga_rp6502/core_bridge_cmd.v"
        "set_global_assignment -name VERILOG_FILE ${APF}/core/pin_ddio_clk.v"
        "set_global_assignment -name SDC_FILE ${APF}/apf/apf_constraints.sdc"
        "set_global_assignment -name SYSTEMVERILOG_FILE ${RP6502_SRC}/fpga/platform/pocket/core_top.sv"
        "set_global_assignment -name VERILOG_FILE ${RP6502_SRC}/fpga/platform/pocket/pocket_pll.v"
        "set_global_assignment -name SDC_FILE ${SYNTH_SDC}"
        "set_global_assignment -name SDC_FILE ${RP6502_SRC}/fpga/platform/pocket/quartus/pocket.sdc"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl/arith"
        # Shift-register recognition was off while M10K was the scarce
        # currency: three blocks went back for a handful of ALMs. The
        # scarcity inverted — the filesystem batch tipped the device to
        # 1853 LABs of 1848 with four blocks idle — so the trade flips
        # with it: the fitter may spend blocks on shift registers again.
        "set_global_assignment -name AUTO_SHIFT_REGISTER_RECOGNITION ON"
        # And area mode: setup closes with more than a nanosecond and a
        # half to spare, which is margin the packer can spend. The old
        # per-knob register packing assignment is gone from 25.1std
        # ("no longer supported -- removing"); the umbrella remains.
        "set_global_assignment -name OPTIMIZATION_MODE \"AGGRESSIVE AREA\"")
    foreach(src ${RP6502_MACHINE_SOURCES})
        if(src MATCHES "\\.sv$")
            list(APPEND BS_LINES
                "set_global_assignment -name SYSTEMVERILOG_FILE ${src}")
        elseif(src MATCHES "\\.v$")
            list(APPEND BS_LINES
                "set_global_assignment -name VERILOG_FILE ${src}")
        else()
            continue()
        endif()
        # The same filter the QSF gets: the machine list also carries the
        # Verilator waivers, and a bitstream that refits because one of
        # those moved is a refit for a file Quartus never opened.
        list(APPEND BS_MACHINE_SOURCES ${src})
    endforeach()
    foreach(src pocket_fifo pocket_video pocket_i2s pocket_sdram pocket_sram
            pocket_bridge pocket_file pocket_bars pocket_dbg pocket_dbglog
            pocket_core)
        list(APPEND BS_LINES
            "set_global_assignment -name SYSTEMVERILOG_FILE ${RP6502_FPGA_POCKET}/${src}.sv")
        list(APPEND BS_POCKET_SOURCES ${RP6502_FPGA_POCKET}/${src}.sv)
    endforeach()
    string(REPLACE ";" "\n" BS_QSF_TEXT "${BS_LINES}")
    file(GENERATE OUTPUT ${BS_QSF} CONTENT "${BS_QSF_TEXT}\n")
    file(WRITE ${BS_DIR}/rp6502.qpf "PROJECT_REVISION = \"rp6502\"\n")
    find_program(QUARTUS_ASM quartus_asm HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
    find_program(QUARTUS_DRC quartus_drc HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
    find_program(QUARTUS_CDB quartus_cdb HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)

    # Everything the fit is made from, which is everything the fit has to be
    # newer than. The machine list already carries the generated packages —
    # the decode table, the palettes, the sine and OPL2 ROMs — so a
    # regenerated asset counts as a moved source without naming it twice.
    #
    # This file stands in for the QSF, which cannot stand for itself: the
    # fitter writes the project file back. It restamps LAST_QUARTUS_VERSION,
    # drops the template's AUTO FIT and HIGH PERFORMANCE EFFORT now that ours
    # override them, and rewrites every path relative. So the QSF differs from
    # what CMake generated the moment a fit ends, the next configure puts ours
    # back, and a fit that depended on it would be out of date again before
    # anyone typed anything. What actually decides the QSF's content is the
    # code below, so that is what the fit is held to.
    set(BS_SOURCES ${BS_MACHINE_SOURCES} ${BS_POCKET_SOURCES}
        ${RP6502_SRC}/fpga/platform/pocket/core_top.sv
        ${RP6502_SRC}/fpga/platform/pocket/pocket_pll.v
        ${RP6502_VENDOR}/openfpga_rp6502/core_bridge_cmd.v
        ${APF}/core/pin_ddio_clk.v
        ${APF}/apf/apf_constraints.sdc
        ${APF}/ap_core.qsf
        ${SYNTH_SDC}
        ${RP6502_SRC}/fpga/platform/pocket/quartus/pocket.sdc
        ${RP6502_SRC}/fpga/platform/pocket/quartus/core_constraints.sdc
        ${CMAKE_CURRENT_LIST_DIR}/synth.cmake)

    # The two gates, written here rather than kept in src/gen, because a
    # gate is mtime arithmetic and CMake does that natively — and writing
    # them here is what lets them carry the file lists CMake already holds
    # instead of a manifest invented for something else to parse. The root
    # CMakeLists generates its version-header script the same way.
    #
    # IS_NEWER_THAN is true when the two stamps are equal, so an exact tie
    # refuses. That is the direction to be wrong in: a needless refit costs
    # ten minutes, and a stale one measures a design nobody wrote.
    string(REPLACE ";" "\"\n    \"" FIT_SOURCE_LINES "${BS_SOURCES}")
    file(WRITE ${BS_DIR}/fit_fresh.cmake
"# Generated by tests/synth.cmake. Edits here are overwritten.\n"
"set(FIT_RPT \"${BS_DIR}/output_files/rp6502.fit.rpt\")\n"
"set(FIT_SOURCES\n    \"${FIT_SOURCE_LINES}\")\n"
[[
# Refuse to put firmware into a fit that no longer describes the tree.
#
# The fast path reassembles the placement that is already on disk, and that
# is only honest while nothing but the firmware has moved. Editing rv_soc.sv
# and reaching for it would otherwise ship last week's RTL carrying today's
# software, with a working bitstream to prove it and nothing anywhere saying
# which halves it was made of.
if(NOT EXISTS "${FIT_RPT}")
    message(FATAL_ERROR
        "fit_fresh: no fit to put firmware into - run `bitstream` first")
endif()
set(FIT_MOVED "")
foreach(src IN LISTS FIT_SOURCES)
    if("${src}" IS_NEWER_THAN "${FIT_RPT}")
        list(APPEND FIT_MOVED "${src}")
    endif()
endforeach()
if(FIT_MOVED)
    string(REPLACE ";" "\n  " FIT_MOVED "${FIT_MOVED}")
    message(FATAL_ERROR
        "fit_fresh: these are newer than the fit, or gone:\n  ${FIT_MOVED}\n"
        "The firmware is not the only thing that changed, so this fit "
        "cannot carry it - run `bitstream`.")
endif()
message(STATUS "fit_fresh: nothing but the firmware has moved since the fit")
]])

    file(WRITE ${BS_DIR}/rbf_fresh.cmake
"# Generated by tests/synth.cmake. Edits here are overwritten.\n"
"set(RBF \"${BS_DIR}/output_files/rp6502.rbf\")\n"
"set(RBF_R \"${BS_DIR}/core.bin\")\n"
[[
# Refuse to package a bitstream older than the fit it claims to be.
#
# The package target copies the reversed bitstream and does not depend on
# the bitstream target, deliberately: assembling a card should not cost a
# refit. The cost of that is a build where the fit ran, a gate after it
# failed, rbf_r_gen never ran, and the copy took the previous one without a
# word. That happened - a bitstream from twenty minutes and one RTL change
# earlier was packaged and very nearly tested, which would have measured the
# wrong design and sent the search somewhere it did not belong.
#
# The assembler's own output is the witness: if output_files/*.rbf is newer
# than the .rbf_r beside it, the reverser did not run for this fit.
foreach(f "${RBF}" "${RBF_R}")
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR "rbf_fresh: ${f} missing - run `bitstream` first")
    endif()
endforeach()
if("${RBF}" IS_NEWER_THAN "${RBF_R}")
    message(FATAL_ERROR
        "rbf_fresh: core.bin is older than rp6502.rbf - the last fit did "
        "not finish its gates, so this would package the bitstream before it")
endif()
message(STATUS "rbf_fresh: the bitstream is the one this fit made")
]])
    # Everything the SD card needs, assembled. The JSONs, the images and
    # the tree come from dist/ as they are; the bitstream and the glyph
    # asset are built. dist/ carries Saves/rp6502/common/ because the
    # host will not create it and the drive is nothing without it.
    set(PKG_DIR ${CMAKE_CURRENT_BINARY_DIR}/package)
    set(PKG_DIST ${RP6502_SRC}/fpga/platform/pocket/dist)
    add_custom_target(package
        # Never package a bitstream older than the fit it claims to be.
        COMMAND ${CMAKE_COMMAND} -P ${BS_DIR}/rbf_fresh.cmake
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${PKG_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PKG_DIST} ${PKG_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory
            ${PKG_DIR}/Assets/rp6502/common
        COMMAND ${CMAKE_COMMAND} -E copy ${VID_FONT_BIN}
            ${PKG_DIR}/Assets/rp6502/common/fonts.bin
        COMMAND ${CMAKE_COMMAND} -E copy ${OEMCP_BIN}
            ${PKG_DIR}/Assets/rp6502/common/oemcp.bin
        COMMAND ${CMAKE_COMMAND} -E copy ${BS_DIR}/core.bin
            ${PKG_DIR}/Cores/Rumbledethumps.RP6502/core.bin
        DEPENDS vid_font_rom
        COMMENT "Assembling the Pocket core package (run `bitstream` first)"
        VERBATIM)

    # The firmware has to be IN the bitstream. Simulation loads it into
    # the verilated arrays from C++, so nothing in the synthesis path
    # ever needed it and for a long time nothing supplied it — the soft
    # CPU came up fetching zeros.
    #
    # A rule of its own rather than the first line of the fit, so the
    # lanes have one writer and the fit has a dependency it can see.
    set(TCM_LANES ${BS_DIR}/sw.0 ${BS_DIR}/sw.1 ${BS_DIR}/sw.2 ${BS_DIR}/sw.3)
    add_custom_command(OUTPUT ${TCM_LANES}
        COMMAND python3 ${RP6502_SRC}/gen/rv_tcm_gen.py
            ${SW_BIN} ${BS_DIR}/sw ${TCM_WORDS}
        DEPENDS ${SW_BIN} ${RP6502_SRC}/gen/rv_tcm_gen.py
        COMMENT "Splitting the firmware into TCM byte lanes"
        VERBATIM)

    add_custom_command(OUTPUT ${BS_DIR}/core.bin
        COMMAND ${QUARTUS_MAP} rp6502
        COMMAND ${QUARTUS_FIT} rp6502
        COMMAND ${QUARTUS_ASM} rp6502
        COMMAND ${QUARTUS_STA} rp6502
        # No .rbf_r from a fit that did not close. A bitstream that
        # misses timing assembles and runs, and only stops running when
        # the part is warm or the fitter's luck turns.
        COMMAND python3 ${RP6502_SRC}/gen/sta_gate.py
            ${BS_DIR}/output_files/rp6502.sta.rpt
        # Nor from one that grew a violation timing cannot see. An
        # unsynchronised reset and a torn opcode both close every corner
        # and both fail on a different fit; the Design Assistant is the
        # only thing in the flow that looks for them.
        COMMAND ${QUARTUS_DRC} rp6502
        COMMAND python3 ${RP6502_SRC}/gen/drc_gate.py
            ${BS_DIR}/output_files/rp6502.drc.rpt
            ${RP6502_SRC}/fpga/platform/pocket/quartus/drc_baseline.txt
        COMMAND python3 ${RP6502_SRC}/gen/rbf_r_gen.py
            ${BS_DIR}/output_files/rp6502.rbf
            ${BS_DIR}/core.bin
        WORKING_DIRECTORY ${BS_DIR}
        DEPENDS ${TCM_LANES} ${BS_SOURCES}
            ${RP6502_SRC}/gen/sta_gate.py ${RP6502_SRC}/gen/drc_gate.py
            ${RP6502_SRC}/gen/rbf_r_gen.py
            ${RP6502_SRC}/fpga/platform/pocket/quartus/drc_baseline.txt
        COMMENT "Building the Pocket bitstream"
        VERBATIM)
    add_custom_target(bitstream DEPENDS ${BS_DIR}/core.bin)

    # The same bitstream with different firmware in it, for the loop that
    # is nearly all of development: change some C, look at the machine.
    #
    # The firmware is not logic. It is the initial contents of four M10K
    # arrays, so a new image places nothing, routes nothing and moves no
    # timing arc — which is why the fitter can be skipped and why the
    # gates it already passed still hold. Quartus keeps its own MIF of
    # each array under db/; rv_mif_gen rewrites those four, --update_mif
    # takes them into the database, and the assembler makes a programming
    # file out of the placement that was there all along. Ten minutes
    # becomes under one.
    #
    # No sta_gate and no drc_gate here on purpose: rerunning them would
    # re-measure the fit that already cleared them. fit_fresh is what
    # makes that a fact rather than an assumption.
    if(QUARTUS_CDB)
        add_custom_target(bitstream_sw
            COMMAND ${CMAKE_COMMAND} -P ${BS_DIR}/fit_fresh.cmake
            COMMAND python3 ${RP6502_SRC}/gen/rv_mif_gen.py
                ${SW_BIN} ${BS_DIR}/db ${TCM_WORDS}
            COMMAND ${QUARTUS_CDB} rp6502 --update_mif
            COMMAND ${QUARTUS_ASM} rp6502
            COMMAND python3 ${RP6502_SRC}/gen/rbf_r_gen.py
                ${BS_DIR}/output_files/rp6502.rbf
                ${BS_DIR}/core.bin
            WORKING_DIRECTORY ${BS_DIR}
            DEPENDS ${TCM_LANES}
            COMMENT "Putting new firmware into the last fit's bitstream"
            VERBATIM)
    else()
        message(STATUS
            "quartus_cdb not found - no bitstream_sw target.")
    endif()
    elseif(NOT EXISTS ${APF}/ap_core.qsf)
        message(STATUS
            "vendor/openfpga absent - no bitstream target. "
            "git submodule update --init vendor/openfpga")
    else()
        message(STATUS
            "no RISC-V toolchain - no bitstream target. "
            "apt install gcc-riscv64-unknown-elf")
    endif()

    add_custom_target(synth_pocket
        COMMAND ${QUARTUS_MAP} pocket
        COMMAND ${QUARTUS_FIT} pocket
        COMMAND ${QUARTUS_STA} pocket
        WORKING_DIRECTORY ${PSYNTH_DIR}
        DEPENDS cpu65_rom vid_font_rom vid_palette_rom aud_sine_rom
        COMMENT "Synthesizing the whole Pocket core"
        VERBATIM)
endif()
