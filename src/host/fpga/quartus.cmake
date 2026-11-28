# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The Quartus tools and the gates every FPGA host's fit passes through.

include_guard(GLOBAL)

# One Quartus at a time. A fit takes most of the machine's memory and every
# core it is given, so two at once take longer than one after the other.
set_property(GLOBAL APPEND PROPERTY JOB_POOLS quartus=1)

# rp6502_quartus_tools(<prefix> HINTS <dir> [VERSION <regex>])
#
# Sets <prefix>_MAP, _FIT, _STA, _ASM, _DRC and _SH to quartus_map and the
# rest, all from the folder quartus_map is found in: HINTS first, then the
# PATH. With VERSION, a Quartus whose --version output does not match the
# regex is left out, and every variable reads NOTFOUND.
function(rp6502_quartus_tools prefix)
    cmake_parse_arguments(PARSE_ARGV 1 Q "" "HINTS;VERSION" "")
    find_program(${prefix}_MAP quartus_map HINTS ${Q_HINTS})
    set(tools FIT STA ASM DRC SH)
    if(${prefix}_MAP)
        get_filename_component(bin ${${prefix}_MAP} DIRECTORY)
        foreach(tool ${tools})
            string(TOLOWER ${tool} name)
            find_program(${prefix}_${tool} quartus_${name}
                HINTS ${bin} NO_DEFAULT_PATH)
        endforeach()
    endif()
    if(${prefix}_MAP AND Q_VERSION)
        execute_process(COMMAND ${${prefix}_MAP} --version
            OUTPUT_VARIABLE version ERROR_QUIET)
        if(NOT version MATCHES "${Q_VERSION}")
            string(REGEX MATCH "Version [^\n]*" version "${version}")
            message(STATUS "${${prefix}_MAP} is ${version}, not "
                "${Q_VERSION} - no ${prefix} targets.")
            foreach(tool MAP ${tools})
                set(${prefix}_${tool} ${prefix}_${tool}-NOTFOUND PARENT_SCOPE)
            endforeach()
        endif()
    endif()
endfunction()

# rp6502_quartus_gates(NAME <name> DIR <dir> PROJECT <project> TOOLS <prefix>
#     MAX_M10K <blocks> [MAX_REGISTERS <n>] [ROOT <dir>]
#     DRC_BASELINE <file> COUNTS <file> [IGNORED <file>]
#     FLOW COMMAND ... [FIT COMMAND ...] DEPENDS ...)
#
# Compiles the Quartus project <project> in <dir> with the commands a host
# gives, and fails the build on whatever its reports show is wrong. FLOW
# runs first and must leave the synthesis report; map_gate checks it, then
# FIT runs. A host that compiles in steps passes its synthesis as FLOW and
# the rest as FIT, so a design that cannot fit stops before the fitter
# starts; a host that runs the stock flow passes all of it as FLOW. By the
# end of FIT the timing analyzer must have run. TOOLS is the prefix
# rp6502_quartus_tools() was given. DEPENDS is what the compile reads.
#
# <dir>/fit.stamp is the compile and every report the gates read, the
# Design Assistant's among them, so a host seeds DRC_BASELINE from it.
# <dir>/signoff.stamp is the gates; a host builds what it ships from it:
#
# - map_gate: the registers and the block memory against the device, and
#   with ROOT, every source file Quartus read lies under ROOT;
# - sta_paths.tcl: the worst paths of every corner, and what the netlist
#   matches for each instance assignment and each register collection in
#   the machine's SDC;
# - sta_gate: every corner's slack, no dropped constraint, and the
#   collections' sizes against COUNTS;
# - drc_gate: the Design Assistant's findings against DRC_BASELINE;
# - assignment_gate: no assignment Quartus ignored or that matches nothing,
#   but those IGNORED lists.
function(rp6502_quartus_gates)
    cmake_parse_arguments(PARSE_ARGV 0 G ""
        "NAME;DIR;PROJECT;TOOLS;MAX_M10K;MAX_REGISTERS;ROOT;DRC_BASELINE;COUNTS;IGNORED"
        "FLOW;FIT;DEPENDS")
    set(gen ${RP6502_SRC}/host/fpga/gen)
    set(tcl ${RP6502_SRC}/host/fpga/quartus/sta_paths.tcl)
    set(rpt ${G_DIR}/output_files/${G_PROJECT})
    set(map_gate_args --max-m10k ${G_MAX_M10K})
    if(G_MAX_REGISTERS)
        list(APPEND map_gate_args --max-registers ${G_MAX_REGISTERS})
    endif()
    if(G_ROOT)
        list(APPEND map_gate_args --root ${G_ROOT})
    endif()
    set(ignored_args)
    if(G_IGNORED)
        set(ignored_args --ignored ${G_IGNORED})
    endif()

    add_custom_command(OUTPUT ${G_DIR}/fit.stamp
        ${G_FLOW}
        COMMAND ${RP6502_PYTHON} ${gen}/map_gate.py ${rpt}.map.rpt
            ${map_gate_args}
        ${G_FIT}
        COMMAND ${${G_TOOLS}_STA} -t ${tcl} ${G_PROJECT} ${RP6502_RTL_SDC}
        COMMAND ${${G_TOOLS}_DRC} ${G_PROJECT}
        COMMAND ${CMAKE_COMMAND} -E touch ${G_DIR}/fit.stamp
        WORKING_DIRECTORY ${G_DIR}
        DEPENDS ${G_DEPENDS} ${gen}/map_gate.py ${tcl} ${RP6502_RTL_SDC}
        COMMENT "Fitting the ${G_NAME} core"
        JOB_POOL quartus
        VERBATIM)

    add_custom_command(OUTPUT ${G_DIR}/signoff.stamp
        COMMAND ${RP6502_PYTHON} ${gen}/sta_gate.py ${rpt}.sta.rpt
            ${rpt}.paths.rpt --names ${rpt}.names.rpt --counts ${G_COUNTS}
        COMMAND ${RP6502_PYTHON} ${gen}/drc_gate.py ${rpt}.drc.rpt
            ${G_DRC_BASELINE}
        COMMAND ${RP6502_PYTHON} ${gen}/assignment_gate.py ${rpt}.map.rpt
            ${rpt}.fit.rpt ${rpt}.names.rpt ${ignored_args}
        COMMAND ${CMAKE_COMMAND} -E touch ${G_DIR}/signoff.stamp
        WORKING_DIRECTORY ${G_DIR}
        DEPENDS ${G_DIR}/fit.stamp ${gen}/sta_gate.py ${gen}/drc_gate.py
            ${gen}/assignment_gate.py ${G_DRC_BASELINE} ${G_COUNTS}
            ${G_IGNORED}
        COMMENT "Signing off the ${G_NAME} fit"
        JOB_POOL quartus
        VERBATIM)
endfunction()
