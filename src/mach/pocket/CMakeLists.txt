# The Pocket: Analogue's framework on top of the machine, a bitstream at the
# end of it, and the SD card tree that carries it.
#
# Everything here needs Quartus, the APF submodule and the soft CPU firmware.
# None of it needs Verilator: the source list comes from machine.cmake, which
# is a list either way. MiSTer gets a directory beside this one.
#
# NONE, because nothing here runs a compiler for this machine. The only
# compiler in the graph is the RISC-V one machine.cmake finds for the soft
# CPU's firmware, and it is invoked by a custom command.

cmake_minimum_required(VERSION 3.21)

project(rp6502-pocket NONE)

cmake_path(SET RP6502_ROOT NORMALIZE ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(RP6502_SRC ${RP6502_ROOT}/src)
set(RP6502_VENDOR ${RP6502_ROOT}/vendor)

# -DRP6502_VERSION / -DRP6502_CI reach core.json's metadata, read by the same
# version.cmake the firmware and the emulator use.
include(${RP6502_ROOT}/version.cmake)

# The machine as a list of files, its generated packages, and the soft CPU's
# firmware; then the Quartus command line the projects below run.
include(${RP6502_SRC}/core/machine.cmake)
include(${RP6502_SRC}/core/quartus.cmake)
include(${RP6502_SRC}/core/synth.cmake)

if(QUARTUS_MAP AND QUARTUS_FIT AND QUARTUS_STA)
    set(POCKET_DIR ${CMAKE_BINARY_DIR}/bitstream)
    # The real thing: Analogue's framework on top, their pin assignments,
    # their apf_top as the root, and a bitstream at the end of it. The
    # pins and device come from the vendored project verbatim — only its
    # own source list is dropped, because those files are what we
    # replace.
    set(BS_QSF ${POCKET_DIR}/rp6502.qsf)
    set(APF ${RP6502_VENDOR}/openfpga/src/fpga)
    # Analogue's framework is only wanted once Quartus is here, which is why
    # this is asked for inside that guard rather than beside the rest.
    rp6502_submodule(vendor/openfpga SENTINEL src/fpga/ap_core.qsf
        WANTS "the Pocket bitstream" OPTIONAL RESULT RP6502_HAVE_APF)
    # The array the firmware is loaded into is sized in the RTL, so read
    # the size from there rather than keeping a second copy in step.
    file(STRINGS ${RP6502_SRC}/core/machine/rp6502_pkg.sv TCM_WORDS_LINE
        REGEX "localparam int RP6502_TCM_WORDS")
    # The name has digits in it, so match the value and not the first
    # number on the line.
    string(REGEX MATCH "= *([0-9]+)" _tcm_m "${TCM_WORDS_LINE}")
    set(TCM_WORDS ${CMAKE_MATCH_1})

    # No firmware, no bitstream. A bitstream without it boots to a soft
    # CPU fetching zeros, which looks like dead hardware and is not.
    if(RP6502_HAVE_APF AND SW_BIN AND QUARTUS_ASM AND QUARTUS_CDB)
    file(MAKE_DIRECTORY ${POCKET_DIR})
    # apf_constraints.sdc reads core/core_constraints.sdc relative to the
    # project directory — the framework's hook for a core's own groups.
    # Stage it where that read looks, instead of listing it a second time
    # and letting the vendor's read fail as a Critical Warning every run.
    # Ours, not the template's: vendor/openfpga's copy groups a PLL this
    # core does not instantiate, so its four filters matched nothing.
    file(COPY ${RP6502_SRC}/host/pocket/quartus/core_constraints.sdc
        DESTINATION ${POCKET_DIR}/core)
    file(STRINGS ${APF}/ap_core.qsf BS_TEMPLATE)
    set(BS_LINES "")
    foreach(line ${BS_TEMPLATE})
        if(NOT line MATCHES "_FILE|SEARCH_PATH|SIGNALTAP")
            list(APPEND BS_LINES "${line}")
        endif()
    endforeach()
    list(APPEND BS_LINES
        "set_global_assignment -name GENERATE_RBF_FILE ON"
        # Quartus fits on one processor unless told otherwise, and a fit is
        # the longest thing this project builds. Intel's parallelism is
        # inside a stage rather than across them, so the result does not
        # depend on the count — check the slack against a known fit if that
        # ever looks doubtful.
        "set_global_assignment -name NUM_PARALLEL_PROCESSORS ALL"
        # Analogue's template asks for Auto Fit, which stops optimizing
        # the moment timing is met and leaves hold sitting wherever it
        # first closed. Standard Fit keeps going, and the margin it finds
        # is what survives the next fit's placement.
        "set_global_assignment -name FITTER_EFFORT \"STANDARD FIT\""
        # Name the synchronisers. Quartus finds the chains on its own and
        # then discards them — "the design MTBF is not calculated because
        # there are no specified synchronizers" — so every crossing into
        # a chain's own first stage was reported as unsynchronised, and
        # the design had no reliability figure at all. STA prints the
        # chain count and the MTBF once this is set; read it there rather
        # than from a number written down here. IF ASYNCHRONOUS rather
        # than FORCED because the clk_sys and
        # clk_rv pair is deliberately one synchronous group: forcing
        # would invent synchronisers across a seam that is not a
        # crossing.
        "set_global_assignment -name SYNCHRONIZER_IDENTIFICATION \"FORCED IF ASYNCHRONOUS\""
        # Put the SRAM's launch and capture flops in the pads. The 6502's
        # four clocks less the chip's tAA is what both pad crossings have
        # to share; an IOE flop is a fixed short hop where a fabric one
        # is a route the fitter re-finds every placement, and the budget
        # does not cover the fabric version.
        )
    foreach(pin sram_a sram_dq sram_oe_n sram_we_n sram_ub_n sram_lb_n)
        list(APPEND BS_LINES
            "set_instance_assignment -name FAST_OUTPUT_REGISTER ON -to ${pin}")
    endforeach()
    # The SDRAM's read return is the tight direction and always was: the
    # memory clock is forwarded half a period late so the chip has time
    # to sample what we launch, which charges the return path for it.
    # What is left after the chip's tAC does not cover a fabric flop's
    # route, so these go in the pads for the same reason the SRAM's do.
    foreach(pin dram_a dram_ba dram_dq dram_dqm dram_cke
            dram_ras_n dram_cas_n dram_we_n)
        list(APPEND BS_LINES
            "set_instance_assignment -name FAST_OUTPUT_REGISTER ON -to ${pin}")
    endforeach()
    list(APPEND BS_LINES
        "set_instance_assignment -name FAST_INPUT_REGISTER ON -to dram_dq"
        "set_instance_assignment -name FAST_INPUT_REGISTER ON -to sram_dq"
        "set_global_assignment -name SEARCH_PATH ${POCKET_DIR}"
        "set_global_assignment -name QIP_FILE ${APF}/apf/apf.qip"
        "set_global_assignment -name VERILOG_FILE ${RP6502_VENDOR}/openfpga_rp6502/core_bridge_cmd.v"
        "set_global_assignment -name VERILOG_FILE ${APF}/core/pin_ddio_clk.v"
        "set_global_assignment -name SDC_FILE ${APF}/apf/apf_constraints.sdc"
        "set_global_assignment -name SYSTEMVERILOG_FILE ${RP6502_SRC}/host/pocket/core_top.sv"
        "set_global_assignment -name VERILOG_FILE ${RP6502_SRC}/host/pocket/pocket_pll.v"
        "set_global_assignment -name SDC_FILE ${RP6502_SDC}"
        "set_global_assignment -name SDC_FILE ${RP6502_SRC}/host/pocket/quartus/pocket.sdc"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl/arith"
        # A shift register the fitter recognises becomes a block. Held
        # off so the trade is not made behind the report: what the
        # design costs in logic should read as logic.
        "set_global_assignment -name AUTO_SHIFT_REGISTER_RECOGNITION OFF"
        # Quartus's own default, stated because Analogue's template asks
        # for HIGH PERFORMANCE EFFORT and something has to displace it.
        # Neither bias is ours to want: a mode that trades area for
        # timing or timing for area is worth setting when a fit is
        # actually short of one of them, and this one is short of
        # neither.
        "set_global_assignment -name OPTIMIZATION_MODE \"BALANCED\"")
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
            pocket_bridge pocket_file pocket_sst pocket_bars pocket_dbg
            pocket_dbglog
            pocket_core)
        list(APPEND BS_LINES
            "set_global_assignment -name SYSTEMVERILOG_FILE ${RP6502_HOST_POCKET}/${src}.sv")
        list(APPEND BS_POCKET_SOURCES ${RP6502_HOST_POCKET}/${src}.sv)
    endforeach()
    string(REPLACE ";" "\n" BS_QSF_TEXT "${BS_LINES}")
    file(GENERATE OUTPUT ${BS_QSF} CONTENT "${BS_QSF_TEXT}\n")
    file(WRITE ${POCKET_DIR}/rp6502.qpf "PROJECT_REVISION = \"rp6502\"\n")

    # Everything the fit is made from, which is everything the fit has to be
    # newer than. The machine list already carries the generated packages —
    # the decode table, the palettes, the sine and OPL2 ROMs — so a
    # regenerated asset counts as a moved source without naming it twice.
    #
    # These three files stand in for the QSF, which cannot stand for itself: the
    # fitter writes the project file back. It restamps LAST_QUARTUS_VERSION,
    # drops the template's AUTO FIT and HIGH PERFORMANCE EFFORT now that ours
    # override them, and rewrites every path relative. So the QSF differs from
    # what CMake generated the moment a fit ends, the next configure puts ours
    # back, and a fit that depended on it would be out of date again before
    # anyone typed anything. What actually decides the QSF's content is the
    # code below, so that is what the fit is held to.
    set(BS_SOURCES ${BS_MACHINE_SOURCES} ${BS_POCKET_SOURCES}
        ${RP6502_SRC}/host/pocket/core_top.sv
        ${RP6502_SRC}/host/pocket/pocket_pll.v
        ${RP6502_VENDOR}/openfpga_rp6502/core_bridge_cmd.v
        ${APF}/core/pin_ddio_clk.v
        ${APF}/apf/apf_constraints.sdc
        ${APF}/ap_core.qsf
        ${RP6502_SDC}
        ${RP6502_SRC}/host/pocket/quartus/pocket.sdc
        ${RP6502_SRC}/host/pocket/quartus/core_constraints.sdc
        ${CMAKE_CURRENT_LIST_FILE}
        ${RP6502_SRC}/core/machine.cmake
        ${RP6502_SRC}/core/quartus.cmake)

    # Three costs, three rules, and CMake decides which of them a change
    # has to pay for. Placing and routing this design is nine minutes and
    # turns on the RTL. Putting an image into four M10K arrays is twenty
    # seconds and turns on the firmware. Copying a card tree is instant.
    #
    # That split is also why nothing here checks whether the fit still
    # matches the tree, or whether the bitstream is older than the fit
    # that claims it. Those were gates once, hand-written, because the
    # bitstream was one target that always ran and the package did not
    # depend on it. A rule cannot run against a stale input, so the graph
    # is the check now, and a check the build cannot forget beats one it
    # could.

    # The lanes are what $readmemh reads while mapping. They seed the
    # arrays; the assemble step below replaces their contents in the
    # database, so what is in them does not decide the placement. That is
    # why sw_bin is named as a target rather than a file: it orders the
    # firmware ahead of the fit without making a firmware change cost one.
    add_custom_command(OUTPUT ${POCKET_DIR}/fit.stamp
        COMMAND python3 ${RP6502_SRC}/core/gen/rv_tcm_gen.py
            ${SW_BIN} ${POCKET_DIR}/sw ${TCM_WORDS}
        COMMAND ${QUARTUS_MAP} rp6502
        # A design that cannot fit says so in the map's register count;
        # the fitter only says it half an hour slower. This has to run
        # between the two, or it is decoration.
        COMMAND python3 ${RP6502_SRC}/core/gen/map_gate.py
            ${POCKET_DIR}/output_files/rp6502.map.rpt
        COMMAND ${QUARTUS_FIT} rp6502
        COMMAND ${QUARTUS_STA} rp6502
        # The worst paths by name, kept beside the signoff numbers: a
        # gate failure in CI is a fit nobody can reproduce, so the
        # report has to say which pair failed while the fit still
        # exists.
        COMMAND ${QUARTUS_STA} -t
            ${RP6502_SRC}/host/pocket/quartus/sta_paths.tcl
        COMMAND ${CMAKE_COMMAND} -E touch ${POCKET_DIR}/fit.stamp
        WORKING_DIRECTORY ${POCKET_DIR}
        DEPENDS ${BS_SOURCES} sw_bin
            ${RP6502_SRC}/core/gen/rv_tcm_gen.py
            ${RP6502_SRC}/core/gen/map_gate.py
            ${RP6502_SRC}/host/pocket/quartus/sta_paths.tcl
        COMMENT "Fitting the Pocket core"
        VERBATIM)

    # Signoff is a reading of the fit, not part of making it, and the
    # split is what that distinction costs or saves: a gate script or a
    # baseline entry is a text edit, and a text edit that re-placed the
    # design was nine minutes of fitter spent blessing a comment. This
    # ran for real once, the other way around.
    #
    # No bitstream from a fit that did not close. One that misses
    # timing assembles and runs, and only stops running when the part
    # is warm or the fitter's luck turns. Nor from one that grew a
    # violation timing cannot see: an unsynchronised reset and a torn
    # opcode both close every corner and both fail on a different fit;
    # the Design Assistant is the only thing in the flow that looks
    # for them. A gate that fails leaves no stamp, and signoff is
    # still owed against the same fit the next time anyone asks.
    add_custom_command(OUTPUT ${POCKET_DIR}/signoff.stamp
        COMMAND python3 ${RP6502_SRC}/core/gen/sta_gate.py
            ${POCKET_DIR}/output_files/rp6502.sta.rpt
            ${POCKET_DIR}/output_files/rp6502.paths.rpt
        COMMAND ${QUARTUS_DRC} rp6502
        COMMAND python3 ${RP6502_SRC}/core/gen/drc_gate.py
            ${POCKET_DIR}/output_files/rp6502.drc.rpt
            ${RP6502_SRC}/host/pocket/quartus/drc_baseline.txt
        COMMAND ${CMAKE_COMMAND} -E touch ${POCKET_DIR}/signoff.stamp
        WORKING_DIRECTORY ${POCKET_DIR}
        DEPENDS ${POCKET_DIR}/fit.stamp
            ${RP6502_SRC}/core/gen/sta_gate.py ${RP6502_SRC}/core/gen/drc_gate.py
            ${RP6502_SRC}/host/pocket/quartus/drc_baseline.txt
        COMMENT "Signing off the Pocket fit"
        VERBATIM)
    # The fit alone, named: point a measurement at the same fit the
    # package consumes and the package finds it already paid.
    add_custom_target(pocket-fit DEPENDS ${POCKET_DIR}/signoff.stamp)

    # The firmware has to be IN the bitstream: simulation loads it into the
    # verilated arrays from C++, so nothing in the synthesis path ever
    # needed it, and for a long time nothing supplied it — the soft CPU
    # came up fetching zeros.
    #
    # It is not logic. It is the initial contents of four M10K arrays, so a
    # new image places nothing and routes nothing, and the fit above stands.
    # Quartus keeps a MIF of each array under db/; rv_mif_gen rewrites those
    # four, --update_mif takes them into the database, and the assembler
    # makes a programming file out of the placement that was already there.
    # Verified the only way worth trusting: the same firmware fitted from
    # scratch and assembled this way produce the same bytes.
    add_custom_command(OUTPUT ${POCKET_DIR}/core.bin
        COMMAND python3 ${RP6502_SRC}/core/gen/rv_mif_gen.py
            ${SW_BIN} ${POCKET_DIR}/db ${TCM_WORDS}
        COMMAND ${QUARTUS_CDB} rp6502 --update_mif
        COMMAND ${QUARTUS_ASM} rp6502
        COMMAND python3 ${RP6502_SRC}/core/gen/rbf_r_gen.py
            ${POCKET_DIR}/output_files/rp6502.rbf
            ${POCKET_DIR}/core.bin
        WORKING_DIRECTORY ${POCKET_DIR}
        DEPENDS ${POCKET_DIR}/signoff.stamp ${SW_BIN}
            ${RP6502_SRC}/core/gen/rv_mif_gen.py ${RP6502_SRC}/core/gen/rbf_r_gen.py
        COMMENT "Putting the firmware into the Pocket bitstream"
        VERBATIM)
    add_custom_target(pocket-bitstream DEPENDS ${POCKET_DIR}/core.bin)

    # Everything the SD card needs, assembled. The JSONs, the images and
    # the tree come from dist/ beside this file as they are; the bitstream and
    # the glyph asset are built. It carries Saves/rp6502/common/ because the
    # host will not create it and the drive is nothing without it.
    #
    # The shipped tree lives with the root that packages it, the way every
    # machine's does. Nothing in it is SystemVerilog or Quartus; it is a
    # folder of JSON and artwork that goes on a card.
    set(PKG_DIR ${CMAKE_BINARY_DIR}/package)
    set(PKG_DIST ${CMAKE_CURRENT_LIST_DIR}/dist)
    file(GLOB_RECURSE PKG_DIST_FILES ${PKG_DIST}/*)
    add_custom_command(OUTPUT ${POCKET_DIR}/package.stamp
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${PKG_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PKG_DIST} ${PKG_DIR}
        COMMAND ${CMAKE_COMMAND}
            -DCORE_JSON=${PKG_DIR}/Cores/Rumbledethumps.RP6502/core.json
            "-DVERSION=${RP6502_VERSION_VALUE}"
            "-DCI=${RP6502_CI_VALUE}"
            -P ${CMAKE_CURRENT_LIST_DIR}/stamp_core_json.cmake
        COMMAND ${CMAKE_COMMAND} -E make_directory
            ${PKG_DIR}/Assets/rp6502/common
        COMMAND ${CMAKE_COMMAND} -E copy ${VID_FONT_BIN}
            ${PKG_DIR}/Assets/rp6502/common/fonts.bin
        COMMAND ${CMAKE_COMMAND} -E copy ${OEMCP_BIN}
            ${PKG_DIR}/Assets/rp6502/common/oemcp.bin
        COMMAND ${CMAKE_COMMAND} -E copy ${KBDLAY_BIN}
            ${PKG_DIR}/Assets/rp6502/common/keyboard.bin
        COMMAND ${CMAKE_COMMAND} -E copy ${POCKET_DIR}/core.bin
            ${PKG_DIR}/Cores/Rumbledethumps.RP6502/core.bin
        COMMAND ${CMAKE_COMMAND} -E touch ${POCKET_DIR}/package.stamp
        DEPENDS ${POCKET_DIR}/core.bin ${VID_FONT_BIN} ${OEMCP_BIN} ${KBDLAY_BIN}
            ${PKG_DIST_FILES}
            ${CMAKE_CURRENT_LIST_DIR}/stamp_core_json.cmake
        COMMENT "Assembling the Pocket core package"
        VERBATIM)
    add_custom_target(pocket ALL DEPENDS ${POCKET_DIR}/package.stamp)

    elseif(NOT RP6502_HAVE_APF)
        message(STATUS "vendor/openfpga absent - no pocket target.")
    elseif(NOT SW_BIN)
        message(STATUS
            "no RISC-V toolchain - no pocket target. "
            "apt install gcc-riscv64-unknown-elf")
    else()
        message(STATUS
            "quartus_asm or quartus_cdb missing - no pocket target.")
    endif()
endif()
