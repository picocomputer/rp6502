include_guard(GLOBAL)

# Inside a function body CMAKE_CURRENT_LIST_DIR names the directory of the
# calling file, so this file's directory is captured when it is included.
set(RP6502_CORE_DIR ${CMAKE_CURRENT_LIST_DIR})

# The interpreter is found rather than named python3 because on Windows it is
# python.exe, and the libretro buildbot runs the OEM code page and resampler
# generators on Windows.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
cmake_path(SET RP6502_VENDOR_DIR NORMALIZE ${CMAKE_CURRENT_LIST_DIR}/../../vendor)

function(rp6502_gen_rsmp_coef target)
    set(gen ${RP6502_CORE_DIR}/gen/rsmp_coef_gen.py)
    set(out ${CMAKE_CURRENT_BINARY_DIR}/rsmp_coef.h)
    add_custom_command(OUTPUT ${out}
        COMMAND ${Python3_EXECUTABLE} ${gen} --emit-h ${out}
        DEPENDS ${gen}
        COMMENT "Generating the resampler coefficients"
        VERBATIM)
    add_custom_target(${target} DEPENDS ${out})
    set(RSMP_COEF_H ${out} PARENT_SCOPE)
    set(RSMP_COEF_DIR ${CMAKE_CURRENT_BINARY_DIR} PARENT_SCOPE)
endfunction()

function(rp6502_gen_oemcp target)
    set(gen ${RP6502_CORE_DIR}/gen/oem_table_gen.py)
    set(ff ${RP6502_VENDOR_DIR}/fatfs/ffunicode.c)
    set(c ${CMAKE_CURRENT_BINARY_DIR}/oemcp.c)
    set(h ${CMAKE_CURRENT_BINARY_DIR}/oemcp.h)
    add_custom_command(OUTPUT ${c} ${h}
        COMMAND ${Python3_EXECUTABLE} ${gen}
            --ffunicode ${ff} --emit-c ${c} --emit-h ${h}
        DEPENDS ${gen} ${ff}
        COMMENT "Generating the OEM code page tables"
        VERBATIM)
    add_custom_target(${target} DEPENDS ${c} ${h})
    set(OEMCP_C ${c} PARENT_SCOPE)
    set(OEMCP_H ${h} PARENT_SCOPE)
    set(OEMCP_DIR ${CMAKE_CURRENT_BINARY_DIR} PARENT_SCOPE)
endfunction()

set(KBDLAY_MANIFEST ${RP6502_CORE_DIR}/def/keyboard.def)
file(GLOB KBDLAY_DEFS ${RP6502_CORE_DIR}/def/keyboard_*.def)

function(rp6502_gen_kbdlay target)
    set(gen ${RP6502_CORE_DIR}/gen/keyboard_layout_gen.py)
    set(c ${CMAKE_CURRENT_BINARY_DIR}/kbdlay.c)
    set(h ${CMAKE_CURRENT_BINARY_DIR}/kbdlay.h)
    add_custom_command(OUTPUT ${c} ${h}
        COMMAND ${Python3_EXECUTABLE} ${gen}
            --manifest ${KBDLAY_MANIFEST} --emit-c ${c} --emit-h ${h}
        DEPENDS ${gen} ${KBDLAY_MANIFEST} ${KBDLAY_DEFS}
        COMMENT "Generating the keyboard layouts"
        VERBATIM)
    add_custom_target(${target} DEPENDS ${c} ${h})
    set(KBDLAY_C ${c} PARENT_SCOPE)
    set(KBDLAY_H ${h} PARENT_SCOPE)
    set(KBDLAY_DIR ${CMAKE_CURRENT_BINARY_DIR} PARENT_SCOPE)
endfunction()
