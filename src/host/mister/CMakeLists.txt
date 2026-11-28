cmake_minimum_required(VERSION 3.21)

project(rp6502-mister NONE)

include(${CMAKE_CURRENT_LIST_DIR}/../../core/build.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/machine.cmake)
include(${RP6502_SRC}/core/rtl.cmake)
include(${RP6502_SRC}/host/fpga/quartus.cmake)
# Releases are built with 17.0.2, as MiSTer's maintainers build, and 25.1,
# the Pocket's Quartus, builds the same tree as a check.
rp6502_quartus_tools(Q17 HINTS $ENV{HOME}/intelFPGA_lite/17.0/quartus/bin
    VERSION "Version 17\\.0\\.2 ")
rp6502_quartus_tools(Q25 HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin
    VERSION "Version 25\\.1")

rp6502_mister_rtl(MISTER_RTL)
rp6502_mister_sys(MISTER_SYS WANTS "every MiSTer target")
if(NOT MISTER_SYS)
    return()
endif()

# The tree is exported in Template_MiSTer's shape, as a MiSTer-devel
# maintainer would check it out: sys/ as upstream has it, the hand-written
# RP6502.qpf, .qsf, .sdc and .sv and the wizard's PLL from tree/, the RTL
# in one flat rtl/, and files.qip listing what the project reads.
set(E ${CMAKE_BINARY_DIR}/RP6502_MiSTer)
set(EXPORT_STAMP ${CMAKE_BINARY_DIR}/export.stamp)

# The core's SDC files. wiring.sdc joins with the machine.
set(MISTER_SDC RP6502.sdc)
set(QIP_LINES)
foreach(src ${MISTER_RTL})
    get_filename_component(name ${src} NAME)
    if(src MATCHES "\\.sv$")
        list(APPEND QIP_LINES "set_global_assignment -name SYSTEMVERILOG_FILE rtl/${name}")
    else()
        list(APPEND QIP_LINES "set_global_assignment -name VERILOG_FILE rtl/${name}")
    endif()
endforeach()
foreach(inc ${RP6502_RTL_INCLUDES})
    get_filename_component(name ${inc} NAME)
    list(APPEND QIP_LINES "set_global_assignment -name SOURCE_FILE rtl/${name}")
endforeach()
foreach(sdc ${MISTER_SDC})
    list(APPEND QIP_LINES "set_global_assignment -name SDC_FILE ${sdc}")
endforeach()
list(APPEND QIP_LINES "set_global_assignment -name SYSTEMVERILOG_FILE RP6502.sv")
string(REPLACE ";" "\n" QIP_TEXT "${QIP_LINES}")
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/files.qip CONTENT "${QIP_TEXT}\n")

file(GLOB_RECURSE TREE_FILES CONFIGURE_DEPENDS ${RP6502_MISTER_TREE}/*)
file(GLOB_RECURSE SYS_FILES ${MISTER_SYS}/*)
set(SDC_GATE_ARGS)
foreach(sdc ${MISTER_SDC})
    list(APPEND SDC_GATE_ARGS --sdc ${E}/${sdc})
endforeach()
add_custom_command(OUTPUT ${EXPORT_STAMP}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${E}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${MISTER_SYS} ${E}/sys
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${RP6502_MISTER_TREE} ${E}
    COMMAND ${CMAKE_COMMAND} -E copy ${MISTER_RTL} ${RP6502_RTL_INCLUDES}
        ${E}/rtl
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/files.qip
        ${E}/files.qip
    COMMAND ${RP6502_PYTHON} ${RP6502_SRC}/host/fpga/gen/sdc_gate.py
        ${SDC_GATE_ARGS} --rtl ${E}/RP6502.sv --rtl-dir ${E}/rtl
    COMMAND ${CMAKE_COMMAND} -E touch ${EXPORT_STAMP}
    DEPENDS ${MISTER_RTL} ${RP6502_RTL_INCLUDES} ${TREE_FILES} ${SYS_FILES}
        ${CMAKE_BINARY_DIR}/files.qip
        ${RP6502_SRC}/host/fpga/gen/sdc_gate.py ${RP6502_PYTHON_LIBS}
    COMMENT "Exporting the MiSTer tree"
    VERBATIM)
add_custom_target(mister_export DEPENDS ${EXPORT_STAMP})

# sys.qip takes the PLL's qip by Quartus's major version, and sys/ has one
# only for 13 and 17. A 25.1 build copy gets pll_q25.qip as a copy of
# pll_q17.qip; sys/ itself, and the export, are never changed.
macro(mister_shim v dir)
    set(shim)
    if(${v} EQUAL 25)
        set(shim COMMAND ${CMAKE_COMMAND} -E copy
            ${dir}/sys/pll_q17.qip ${dir}/sys/pll_q25.qip)
    endif()
endmacro()

# mister_elab analyses and elaborates the exported tree, which takes
# seconds, in a fresh copy for each Quartus. Quartus rewrites a project it
# opens, so neither copy is the export. build_id.v, which RP6502.sv
# includes, is written by the flow's pre-flow script, run here by hand.
set(ELAB_STAMPS)
foreach(v 17 25)
    if(NOT (Q${v}_MAP AND Q${v}_SH))
        continue()
    endif()
    set(dir ${CMAKE_BINARY_DIR}/elab${v})
    mister_shim(${v} ${dir})
    add_custom_command(OUTPUT ${dir}.stamp
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${dir}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${E} ${dir}
        ${shim}
        COMMAND ${CMAKE_COMMAND} -E chdir ${dir}
            ${Q${v}_SH} -t sys/build_id.tcl compile RP6502 RP6502
        COMMAND ${CMAKE_COMMAND} -E chdir ${dir}
            ${Q${v}_MAP} --analysis_and_elaboration RP6502
        COMMAND ${CMAKE_COMMAND} -E touch ${dir}.stamp
        DEPENDS ${EXPORT_STAMP}
        COMMENT "Elaborating the MiSTer tree with Quartus ${v}"
        JOB_POOL quartus
        VERBATIM)
    list(APPEND ELAB_STAMPS ${dir}.stamp)
endforeach()
if(NOT ELAB_STAMPS)
    message(STATUS "Neither Quartus 17.0.2 nor 25.1 - no MiSTer Quartus "
        "targets.")
    return()
endif()
add_custom_target(mister_elab DEPENDS ${ELAB_STAMPS})

# rbf17 and rbf25 build a copy of the export that keeps its db/, with the
# stock flow, as MiSTer's maintainers build it: quartus_sh --flow compile
# runs build_id.tcl, synthesis, the fit, the assembler and the timing
# analyzer. Every file of the export is copied over the last build's, the
# qsf that Quartus rewrote among them. The gates then judge the reports.
# A drc baseline is seeded from this core's own fit with that version;
# until it exists, ninja refuses the rbf target, and the build of q<v>/
# fit.stamp alone gives the fit to seed it from.
foreach(v 17 25)
    if(NOT (Q${v}_MAP AND Q${v}_SH AND Q${v}_STA AND Q${v}_DRC))
        message(STATUS "Quartus ${v} incomplete - no rbf${v} target.")
        continue()
    endif()
    set(baseline ${CMAKE_CURRENT_LIST_DIR}/quartus/drc_baseline${v}.txt)
    if(NOT EXISTS ${baseline})
        message(WARNING "No ${baseline}: rbf${v} cannot run until it is "
            "seeded. Build q${v}/fit.stamp and seed it from "
            "q${v}/output_files/RP6502.drc.rpt.")
    endif()
    set(dir ${CMAKE_BINARY_DIR}/q${v})
    file(MAKE_DIRECTORY ${dir})
    mister_shim(${v} ${dir})
    rp6502_quartus_gates(NAME "MiSTer (Quartus ${v})" DIR ${dir}
        PROJECT RP6502 TOOLS Q${v}
        # the 5CSEBA6U23I7 has 553 M10K blocks
        MAX_M10K 553 MAX_REGISTERS 50000
        ROOT ${dir}
        DRC_BASELINE ${baseline}
        COUNTS ${CMAKE_CURRENT_LIST_DIR}/quartus/wiring_sdc_counts.txt
        IGNORED ${CMAKE_CURRENT_LIST_DIR}/quartus/sys_ignored.txt
        FLOW COMMAND ${CMAKE_COMMAND} -E copy_directory ${E} ${dir}
            ${shim}
            COMMAND ${Q${v}_SH} --flow compile RP6502
        DEPENDS ${EXPORT_STAMP} ${CMAKE_BINARY_DIR}/elab${v}.stamp)
    add_custom_target(rbf${v} DEPENDS ${dir}/signoff.stamp)
endforeach()

# The release: _Computer/RP6502_<date>.rbf as a MiSTer's SD card holds it,
# zipped, and the same rbf in the tree's releases/. It runs after every
# export, which empties releases/.
if(TARGET rbf17)
    set(PACKAGE_STAMP ${CMAKE_BINARY_DIR}/package.stamp)
    add_custom_command(OUTPUT ${PACKAGE_STAMP}
        COMMAND ${CMAKE_COMMAND} -DBUILD=${CMAKE_BINARY_DIR}/q17 -DTREE=${E}
            -DPKG=${CMAKE_BINARY_DIR}/package
            -DZIP=${CMAKE_BINARY_DIR}/RP6502_MiSTer.zip
            -P ${CMAKE_CURRENT_LIST_DIR}/package.cmake
        COMMAND ${CMAKE_COMMAND} -E touch ${PACKAGE_STAMP}
        DEPENDS ${CMAKE_BINARY_DIR}/q17/signoff.stamp ${EXPORT_STAMP}
            ${CMAKE_CURRENT_LIST_DIR}/package.cmake
        COMMENT "Assembling the MiSTer release"
        VERBATIM)
    add_custom_target(package DEPENDS ${PACKAGE_STAMP})
    # The one target a plain build makes: both elaborations, the 17.0.2
    # fit and its release. rbf25 is asked for by name, at every milestone
    # and before every release.
    add_custom_target(mister ALL DEPENDS ${ELAB_STAMPS} ${PACKAGE_STAMP})
endif()
