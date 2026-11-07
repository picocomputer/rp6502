list(PREPEND RP6502_MACHINE_SOURCES
    ${RP6502_BENCH}/hazard3.vlt ${RP6502_BENCH}/machine.vlt)

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
    cmake_parse_arguments(U "SPLIT" "TOP;PREFIX;TIMEOUT"
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
    if(U_SPLIT)
        list(APPEND _args SPLIT)
    endif()
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
        INCLUDES ${RP6502_BENCH} ${RP6502_GEN} ${RP6502_SRC})
    if(M_SPLIT)
        list(APPEND _args SPLIT)
    endif()
    set(_deps ${M_DEPENDS})
    if(M_FIRMWARE)
        list(APPEND _args DEFS SW_BIN="${SW_BIN}"
            FONTS_BIN="${RP6502_ASSETS}/font_rom.bin"
            OEMCP_BIN="${RP6502_ASSETS}/unicode_oem.bin"
            KBDLAY_BIN="${RP6502_ASSETS}/layout_db.bin" ${M_DEFS})
        list(APPEND _deps sw_bin rp6502_gen_assets)
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
        DEPENDS rp6502_gen_assets)
    target_link_libraries(test_${name} PRIVATE ${_model})
endfunction()
