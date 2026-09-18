set(RP6502_TEST_TABLES ${CMAKE_BINARY_DIR}/test_tables)
file(MAKE_DIRECTORY ${RP6502_TEST_TABLES})

function(rp6502_test_table target)
    cmake_parse_arguments(H "" "GEN;COMMENT" "OUTPUTS;ARGS;DEPENDS" ${ARGN})
    add_custom_command(OUTPUT ${H_OUTPUTS}
        COMMAND ${CMAKE_COMMAND} -E env python3 ${H_GEN} ${H_ARGS}
        DEPENDS ${H_GEN} ${H_DEPENDS}
        COMMENT "${H_COMMENT}"
        VERBATIM)
    add_custom_target(${target} DEPENDS ${H_OUTPUTS})
endfunction()

set(VID_FONT_H ${RP6502_TEST_TABLES}/vid_font_tables.h)
rp6502_test_table(vid_font_tables GEN ${RP6502_SRC}/core/gen/vid_font_gen.py
    ARGS --emit-h ${VID_FONT_H}
    OUTPUTS ${VID_FONT_H}
    DEPENDS ${RP6502_SRC}/core/term/font.c
    COMMENT "Generating the font tables test_font reads")

set(VID_PALETTE_H ${RP6502_TEST_TABLES}/vid_palette_tables.h)
rp6502_test_table(vid_palette_tables GEN ${RP6502_SRC}/core/gen/vid_palette_gen.py
    ARGS --emit-h ${VID_PALETTE_H}
    OUTPUTS ${VID_PALETTE_H}
    DEPENDS ${RP6502_SRC}/core/term/color.c
    COMMENT "Generating the palette tables the pixel tests read")

set(AUD_SINE_H ${RP6502_TEST_TABLES}/aud_sine_tables.h)
rp6502_test_table(aud_sine_tables GEN ${RP6502_SRC}/core/gen/aud_sine_gen.py
    ARGS --emit-h ${AUD_SINE_H}
    OUTPUTS ${AUD_SINE_H}
    COMMENT "Generating the sine table the PSG shim reads")

set(OPL2_LUT_H ${RP6502_TEST_TABLES}/opl2_lut_tables.h)
rp6502_test_table(opl2_lut_tables GEN ${RP6502_SRC}/core/gen/opl2_lut_gen.py
    ARGS --log-sine ${OPL2_LUT_SRC}/opl2_log_sine_lut.sv
        --exp ${OPL2_LUT_SRC}/opl2_exp_lut.sv --emit-h ${OPL2_LUT_H}
    OUTPUTS ${OPL2_LUT_H}
    DEPENDS ${OPL2_LUT_SRC}/opl2_log_sine_lut.sv ${OPL2_LUT_SRC}/opl2_exp_lut.sv
    COMMENT "Generating the OPL2 LUT tables test_oplrom reads")

list(PREPEND OPL2_SOURCES ${RP6502_BENCH}/opl2.vlt)
list(PREPEND RP6502_MACHINE_SOURCES
    ${RP6502_BENCH}/hazard3.vlt ${RP6502_BENCH}/opl2.vlt)

function(rp6502_model out)
    cmake_parse_arguments(V "TRACE" "TOP;PREFIX" "RTL;ARGS;INCLUDE_DIRS;DEPENDS" ${ARGN})
    set(_target model_${V_PREFIX})
    if(V_TRACE)
        set(_target ${_target}_trace)
    endif()
    set(${out} ${_target} PARENT_SCOPE)

    set(_key "${V_TOP}|${V_RTL}|${V_ARGS}|${V_INCLUDE_DIRS}")
    if(TARGET ${_target})
        get_target_property(_was ${_target} RP6502_MODEL_KEY)
        if(NOT _was STREQUAL _key)
            message(FATAL_ERROR
                "${_target} was already built from a different design.\n"
                "  had: ${_was}\n"
                "  now: ${_key}")
        endif()
        return()
    endif()

    set(_trace)
    set(_opt -O3)
    if(V_TRACE)
        set(_trace TRACE_FST)
        set(_opt)
    endif()

    add_library(${_target} STATIC)
    set_target_properties(${_target} PROPERTIES RP6502_MODEL_KEY "${_key}")
    verilate(${_target}
        SOURCES ${V_RTL}
        TOP_MODULE ${V_TOP}
        PREFIX ${V_PREFIX}
        ${_trace}
        INCLUDE_DIRS ${V_INCLUDE_DIRS}
        VERILATOR_ARGS -Wall --assert ${_opt} ${V_ARGS})
    if(V_DEPENDS)
        add_dependencies(${_target} ${V_DEPENDS})
    endif()
endfunction()

function(rp6502_add_module_test name)
    cmake_parse_arguments(U "" "TOP;PREFIX;TIMEOUT"
        "RTL;SOURCES;INCLUDES;DEFS;LIBS;DEPENDS;ARGS" ${ARGN})
    if(NOT U_SOURCES)
        set(U_SOURCES test_${name}.cpp)
    endif()
    if(NOT U_PREFIX)
        set(U_PREFIX V${U_TOP})
    endif()
    set(_args SOURCES ${U_SOURCES} LABELS sim module)
    foreach(kw INCLUDES DEFS LIBS TIMEOUT)
        if(U_${kw})
            list(APPEND _args ${kw} ${U_${kw}})
        endif()
    endforeach()
    rp6502_add_test(${name} ${_args})
    if(U_DEPENDS)
        add_dependencies(test_${name} ${U_DEPENDS})
    endif()
    rp6502_model(_model
        TOP ${U_TOP}
        PREFIX ${U_PREFIX}
        RTL ${U_RTL}
        ARGS ${U_ARGS}
        DEPENDS ${U_DEPENDS})
    target_link_libraries(test_${name} PRIVATE ${_model})
endfunction()

function(rp6502_add_machine_test name)
    cmake_parse_arguments(M "TRACE;FIRMWARE;SPLIT" "TOP;PREFIX;TIMEOUT"
        "SOURCES;DEFS;RTL;DEPENDS" ${ARGN})
    if(NOT M_SOURCES)
        set(M_SOURCES test_${name}.cpp)
    endif()
    if(NOT M_TOP)
        set(M_TOP wiring)
    endif()
    if(NOT M_PREFIX)
        set(M_PREFIX V${M_TOP})
    endif()

    set(_labels sim machine)
    if(M_FIRMWARE)
        list(APPEND _labels firmware)
    endif()
    set(_args SOURCES ${M_SOURCES} LABELS ${_labels}
        INCLUDES ${RP6502_BENCH} ${RP6502_ASSETS} ${RP6502_SRC})
    if(M_SPLIT)
        list(APPEND _args SPLIT)
    endif()
    set(_deps ${M_DEPENDS})
    if(M_FIRMWARE)
        list(APPEND _args DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
            OEMCP_BIN="${OEMCP_BIN}" KBDLAY_BIN="${KBDLAY_BIN}" ${M_DEFS})
        list(APPEND _deps sw_bin vid_font_rom vid_palette_rom oemcp_bin
            kbdlay_bin)
    elseif(M_DEFS)
        list(APPEND _args DEFS ${M_DEFS})
    endif()
    if(M_TIMEOUT)
        list(APPEND _args TIMEOUT ${M_TIMEOUT})
    endif()
    rp6502_add_test(${name} ${_args})
    if(_deps)
        add_dependencies(test_${name} ${_deps})
    endif()

    set(_trace)
    if(M_TRACE)
        set(_trace TRACE)
    endif()
    rp6502_model(_model
        TOP ${M_TOP}
        PREFIX ${M_PREFIX}
        RTL ${RP6502_MACHINE_SOURCES} ${M_RTL}
        ${_trace}
        ARGS ${RP6502_RTL_VERILATOR_ARGS}
        INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
        DEPENDS w65c02_rom vid_palette_rom aud_sine_rom opl2_lut_rom rsmp_coef_pkg)
    target_link_libraries(test_${name} PRIVATE ${_model})
endfunction()
