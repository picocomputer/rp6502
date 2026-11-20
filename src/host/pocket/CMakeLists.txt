cmake_minimum_required(VERSION 3.21)

project(rp6502-pocket NONE)

include(${CMAKE_CURRENT_LIST_DIR}/../../core/build.cmake)

include(${RP6502_ROOT}/version.cmake)

set(RP6502_LOG_LEVEL ERROR CACHE STRING
    "What every log category says: NONE, ERROR, WARN, INFO, DEBUG")

include(${CMAKE_CURRENT_LIST_DIR}/machine.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/sw.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/quartus.cmake)

if(QUARTUS_MAP AND QUARTUS_FIT AND QUARTUS_STA)
    set(POCKET_DIR ${CMAKE_BINARY_DIR}/bitstream)
    set(BS_QSF ${POCKET_DIR}/rp6502.qsf)
    set(APF ${RP6502_VENDOR}/openfpga/src/fpga)
    rp6502_submodule(vendor/openfpga SENTINEL src/fpga/ap_core.qsf
        WANTS "the Pocket bitstream" OPTIONAL RESULT RP6502_HAVE_APF)

    if(RP6502_HAVE_APF AND SW_BIN AND QUARTUS_ASM)
    file(MAKE_DIRECTORY ${POCKET_DIR})
    # apf_constraints.sdc reads core/core_constraints.sdc by a path relative to
    # the project directory, so this core's copy is staged at that path.
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
        "set_global_assignment -name NUM_PARALLEL_PROCESSORS ALL"
        "set_global_assignment -name SYNCHRONIZER_IDENTIFICATION \"FORCED IF ASYNCHRONOUS\""
        )
    # The SDRAM's registers are placed in the I/O elements, so the paths to
    # and from its pins are the same on every fit.
    foreach(pin dram_a dram_ba dram_dq dram_dqm dram_cke
            dram_ras_n dram_cas_n dram_we_n)
        list(APPEND BS_LINES
            "set_instance_assignment -name FAST_OUTPUT_REGISTER ON -to ${pin}")
    endforeach()
    # pcm's accumulator feeds a multiplier, and the fitter would pack it
    # into that DSP block as its input register and keep a copy in logic
    # for the adder. The DSP block's clock arrives 0.7 ns after the logic's,
    # so the copy would reach the packed register too soon. The accumulator
    # stays in logic, one register, and the multiplier takes it unregistered.
    list(APPEND BS_LINES
        "set_instance_assignment -name QII_AUTO_PACKED_REGISTERS OFF -to \"*|pcm:pcm|acc[*]\"")
    list(APPEND BS_LINES
        "set_instance_assignment -name FAST_INPUT_REGISTER ON -to dram_dq"
        "set_global_assignment -name SEARCH_PATH ${POCKET_DIR}"
        "set_global_assignment -name QIP_FILE ${APF}/apf/apf.qip"
        "set_global_assignment -name VERILOG_FILE ${RP6502_VENDOR}/openfpga_rp6502/core_bridge_cmd.v"
        "set_global_assignment -name VERILOG_FILE ${APF}/core/pin_ddio_clk.v"
        "set_global_assignment -name SDC_FILE ${APF}/apf/apf_constraints.sdc"
        "set_global_assignment -name SYSTEMVERILOG_FILE ${RP6502_SRC}/host/pocket/core/core_top.sv"
        "set_global_assignment -name VERILOG_FILE ${RP6502_SRC}/host/pocket/core/pocket_pll.v"
        "set_global_assignment -name SDC_FILE ${RP6502_SDC}"
        "set_global_assignment -name SDC_FILE ${RP6502_SRC}/host/pocket/quartus/pocket.sdc"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl/arith"
        "set_global_assignment -name AUTO_SHIFT_REGISTER_RECOGNITION OFF")
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
        list(APPEND BS_MACHINE_SOURCES ${src})
    endforeach()
    foreach(src pocket_fifo pocket_video pocket_i2s pocket_sdram
            pocket_bridge pocket_file pocket_sst pocket_bars pocket_dbg
            pocket_dbglog
            pocket_core)
        list(APPEND BS_LINES
            "set_global_assignment -name SYSTEMVERILOG_FILE ${RP6502_POCKET_CORE}/${src}.sv")
        list(APPEND BS_POCKET_SOURCES ${RP6502_POCKET_CORE}/${src}.sv)
    endforeach()
    string(REPLACE ";" "\n" BS_QSF_TEXT "${BS_LINES}")
    file(GENERATE OUTPUT ${BS_QSF} CONTENT "${BS_QSF_TEXT}\n")
    file(WRITE ${POCKET_DIR}/rp6502.qpf "PROJECT_REVISION = \"rp6502\"\n")

    # rp6502.qsf is left out of BS_SOURCES because Quartus rewrites it during a
    # compile and the next configure writes the generated text back, so a fit
    # that depended on it would rerun after every configure that follows a
    # compile.
    set(BS_SOURCES ${BS_MACHINE_SOURCES} ${BS_POCKET_SOURCES}
        ${RP6502_SRC}/host/pocket/core/core_top.sv
        ${RP6502_SRC}/host/pocket/core/pocket_pll.v
        ${RP6502_VENDOR}/openfpga_rp6502/core_bridge_cmd.v
        ${APF}/core/pin_ddio_clk.v
        ${APF}/apf/apf_constraints.sdc
        ${APF}/ap_core.qsf
        ${RP6502_SDC}
        ${RP6502_SRC}/host/pocket/quartus/pocket.sdc
        ${RP6502_SRC}/host/pocket/quartus/core_constraints.sdc
        ${CMAKE_CURRENT_LIST_FILE}
        ${RP6502_SRC}/core/rtl.cmake
        ${RP6502_SRC}/core/gen.cmake
        ${CMAKE_CURRENT_LIST_DIR}/quartus.cmake)

    # The firmware is not in the bitstream. The host loads it into the SDRAM
    # as a data slot, so nothing here depends on it and a firmware change
    # reaches the package without a Quartus tool running.
    add_custom_command(OUTPUT ${POCKET_DIR}/fit.stamp
        COMMAND ${QUARTUS_MAP} rp6502
        COMMAND ${RP6502_PYTHON} ${RP6502_SRC}/host/pocket/gen/map_gate.py
            ${POCKET_DIR}/output_files/rp6502.map.rpt
        COMMAND ${QUARTUS_FIT} rp6502
        COMMAND ${QUARTUS_STA} rp6502
        COMMAND ${QUARTUS_STA} -t
            ${RP6502_SRC}/host/pocket/quartus/sta_paths.tcl
        COMMAND ${CMAKE_COMMAND} -E touch ${POCKET_DIR}/fit.stamp
        WORKING_DIRECTORY ${POCKET_DIR}
        DEPENDS ${BS_SOURCES}
            ${RP6502_SRC}/host/pocket/gen/map_gate.py
            ${RP6502_SRC}/host/pocket/quartus/sta_paths.tcl
        COMMENT "Fitting the Pocket core"
        VERBATIM)

    add_custom_command(OUTPUT ${POCKET_DIR}/signoff.stamp
        COMMAND ${RP6502_PYTHON} ${RP6502_SRC}/host/pocket/gen/sta_gate.py
            ${POCKET_DIR}/output_files/rp6502.sta.rpt
            ${POCKET_DIR}/output_files/rp6502.paths.rpt
        COMMAND ${QUARTUS_DRC} rp6502
        COMMAND ${RP6502_PYTHON} ${RP6502_SRC}/host/pocket/gen/drc_gate.py
            ${POCKET_DIR}/output_files/rp6502.drc.rpt
            ${RP6502_SRC}/host/pocket/quartus/drc_baseline.txt
        COMMAND ${CMAKE_COMMAND} -E touch ${POCKET_DIR}/signoff.stamp
        WORKING_DIRECTORY ${POCKET_DIR}
        DEPENDS ${POCKET_DIR}/fit.stamp
            ${RP6502_SRC}/host/pocket/gen/sta_gate.py ${RP6502_SRC}/host/pocket/gen/drc_gate.py
            ${RP6502_SRC}/host/pocket/quartus/drc_baseline.txt
        COMMENT "Signing off the Pocket fit"
        VERBATIM)
    add_custom_target(pocket-fit DEPENDS ${POCKET_DIR}/signoff.stamp)

    add_custom_command(OUTPUT ${POCKET_DIR}/core.bin
        COMMAND ${QUARTUS_ASM} rp6502
        COMMAND ${RP6502_PYTHON} ${RP6502_SRC}/host/pocket/gen/rbf_r_gen.py
            ${POCKET_DIR}/output_files/rp6502.rbf
            ${POCKET_DIR}/core.bin
        WORKING_DIRECTORY ${POCKET_DIR}
        DEPENDS ${POCKET_DIR}/signoff.stamp
            ${RP6502_SRC}/host/pocket/gen/rbf_r_gen.py
        COMMENT "Assembling the Pocket bitstream"
        VERBATIM)
    add_custom_target(pocket-bitstream DEPENDS ${POCKET_DIR}/core.bin)

    set(PKG_DIR ${CMAKE_BINARY_DIR}/package)
    set(PKG_DIST ${CMAKE_CURRENT_LIST_DIR}/dist)
    file(GLOB_RECURSE PKG_DIST_FILES ${PKG_DIST}/*)
    add_custom_command(OUTPUT ${POCKET_DIR}/package.stamp
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${PKG_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PKG_DIST} ${PKG_DIR}
        COMMAND ${CMAKE_COMMAND}
            -DCORE_JSON=${PKG_DIR}/Cores/Rumbledethumps.RP6502/core.json
            -DSTAMP_SCRIPT=${RP6502_STAMP_SCRIPT}
            "-DSTAMP_VERSION=${RP6502_VERSION_VALUE}"
            "-DSTAMP_CI=${RP6502_CI_VALUE}"
            "-DSTAMP_GIT=${RP6502_GIT_VALUE}"
            -P ${CMAKE_CURRENT_LIST_DIR}/stamp_core_json.cmake
        COMMAND ${CMAKE_COMMAND} -E make_directory
            ${PKG_DIR}/Assets/rp6502/common
        COMMAND ${CMAKE_COMMAND} -E copy ${SW_BIN}
            ${PKG_DIR}/Assets/rp6502/common/firmware.bin
        COMMAND ${CMAKE_COMMAND} -E copy ${POCKET_DIR}/core.bin
            ${PKG_DIR}/Cores/Rumbledethumps.RP6502/core.bin
        COMMAND ${CMAKE_COMMAND} -E touch ${POCKET_DIR}/package.stamp
        DEPENDS ${POCKET_DIR}/core.bin ${SW_BIN} ${PKG_DIST_FILES}
            ${CMAKE_CURRENT_LIST_DIR}/stamp_core_json.cmake ${RP6502_STAMP_SCRIPT}
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
        message(STATUS "quartus_asm missing - no pocket target.")
    endif()
endif()
