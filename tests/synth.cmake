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
    foreach(src pocket_fifo pocket_video pocket_i2s pocket_sdram
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
        endif()
    endforeach()
    foreach(src pocket_fifo pocket_video pocket_i2s pocket_sdram
            pocket_bridge pocket_file pocket_bars pocket_dbg pocket_dbglog
            pocket_core)
        list(APPEND BS_LINES
            "set_global_assignment -name SYSTEMVERILOG_FILE ${RP6502_FPGA_POCKET}/${src}.sv")
    endforeach()
    string(REPLACE ";" "\n" BS_QSF_TEXT "${BS_LINES}")
    file(WRITE ${BS_QSF} "${BS_QSF_TEXT}\n")
    file(WRITE ${BS_DIR}/rp6502.qpf "PROJECT_REVISION = \"rp6502\"\n")
    find_program(QUARTUS_ASM quartus_asm HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
    find_program(QUARTUS_DRC quartus_drc HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
    # Everything the SD card needs, assembled. The JSONs, the images and
    # the tree come from dist/ as they are; the bitstream and the glyph
    # asset are built. dist/ carries Saves/rp6502/common/ because the
    # host will not create it and the drive is nothing without it.
    set(PKG_DIR ${CMAKE_CURRENT_BINARY_DIR}/package)
    set(PKG_DIST ${RP6502_SRC}/fpga/platform/pocket/dist)
    add_custom_target(package
        # Never package a bitstream older than the fit it claims to be.
        COMMAND python3 ${RP6502_SRC}/gen/rbf_fresh.py
            ${BS_DIR}/output_files/rp6502.rbf ${BS_DIR}/bitstream.rbf_r
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${PKG_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PKG_DIST} ${PKG_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory
            ${PKG_DIR}/Assets/rp6502/common
        COMMAND ${CMAKE_COMMAND} -E copy ${VID_FONT_BIN}
            ${PKG_DIR}/Assets/rp6502/common/fonts.bin
        COMMAND ${CMAKE_COMMAND} -E copy ${OEMCP_BIN}
            ${PKG_DIR}/Assets/rp6502/common/oemcp.bin
        COMMAND ${CMAKE_COMMAND} -E copy ${BS_DIR}/bitstream.rbf_r
            ${PKG_DIR}/Cores/Rumbledethumps.RP6502/bitstream.rbf_r
        DEPENDS vid_font_rom
        COMMENT "Assembling the Pocket core package (run `bitstream` first)"
        VERBATIM)

    # The firmware has to be IN the bitstream. Simulation loads it into
    # the verilated arrays from C++, so nothing in the synthesis path
    # ever needed it and for a long time nothing supplied it — the soft
    # CPU came up fetching zeros.
    add_custom_target(bitstream
        COMMAND python3 ${RP6502_SRC}/gen/rv_tcm_gen.py
            ${SW_BIN} ${BS_DIR}/sw ${TCM_WORDS}
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
            ${BS_DIR}/bitstream.rbf_r
        WORKING_DIRECTORY ${BS_DIR}
        DEPENDS sw_bin cpu65_rom vid_font_rom vid_palette_rom aud_sine_rom
        COMMENT "Building the Pocket bitstream"
        VERBATIM)
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
