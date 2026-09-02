# How each of the core's generated tables is made, for the machines that
# compile one. What a machine does with the file is its own business; where it
# lands is the caller's binary directory.
#
# The .sv and .bin forms of the same tables are core/assets.cmake's. Those are
# staged into a fabric rather than compiled, and they are wanted by one tree,
# so the two mechanisms stay apart.
#
# Each function takes the name of the target to hang the rule on, because a
# tree that wants a table in two forms needs two names for them.

include_guard(GLOBAL)

# Captured here: inside a function body CMAKE_CURRENT_LIST_DIR is the caller's
# file, not this one.
set(RP6502_CORE_DIR ${CMAKE_CURRENT_LIST_DIR})
cmake_path(SET RP6502_VENDOR_DIR NORMALIZE ${CMAKE_CURRENT_LIST_DIR}/../../vendor)

# rp6502_gen_rsmp_coef(<target>) -> RSMP_COEF_H, RSMP_COEF_DIR
#
# The OPL resampler's polyphase coefficients: three thousand numbers nobody can
# check by eye, so they are built rather than committed. Standard library only
# — a windowed sinc needs no solver — so this costs the build nothing but
# python3, which it already needs.
function(rp6502_gen_rsmp_coef target)
    set(gen ${RP6502_CORE_DIR}/gen/rsmp_coef_gen.py)
    set(out ${CMAKE_CURRENT_BINARY_DIR}/rsmp_coef.h)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E env python3 ${gen} --emit-h ${out}
        DEPENDS ${gen}
        COMMENT "Generating the resampler coefficients"
        VERBATIM)
    add_custom_target(${target} DEPENDS ${out})
    set(RSMP_COEF_H ${out} PARENT_SCOPE)
    set(RSMP_COEF_DIR ${CMAKE_CURRENT_BINARY_DIR} PARENT_SCOPE)
endfunction()

# rp6502_gen_oemcp(<target>) -> OEMCP_C, OEMCP_H, OEMCP_DIR
#
# The OEM code page tables, lifted out of vendor/fatfs/ffunicode.c so the logic
# in core/str/unicode.c can be read without a preprocessor.
function(rp6502_gen_oemcp target)
    set(gen ${RP6502_CORE_DIR}/gen/oem_table_gen.py)
    set(ff ${RP6502_VENDOR_DIR}/fatfs/ffunicode.c)
    set(c ${CMAKE_CURRENT_BINARY_DIR}/oemcp.c)
    set(h ${CMAKE_CURRENT_BINARY_DIR}/oemcp.h)
    add_custom_command(OUTPUT ${c} ${h}
        COMMAND ${CMAKE_COMMAND} -E env python3 ${gen}
            --ffunicode ${ff} --emit-c ${c} --emit-h ${h}
        DEPENDS ${gen} ${ff}
        COMMENT "Generating the OEM code page tables"
        VERBATIM)
    add_custom_target(${target} DEPENDS ${c} ${h})
    set(OEMCP_C ${c} PARENT_SCOPE)
    set(OEMCP_H ${h} PARENT_SCOPE)
    set(OEMCP_DIR ${CMAKE_CURRENT_BINARY_DIR} PARENT_SCOPE)
endfunction()

# The keyboard layouts, out of core/def/keyboard_*.def. The manifest names the
# layouts and their order, so a menu that picks one by position and an image
# that declares its own size are both held to it -- it is set here rather than
# by each caller, so there is one answer to which file that is.
set(KBDLAY_MANIFEST ${RP6502_CORE_DIR}/def/keyboard.def)
file(GLOB KBDLAY_DEFS ${RP6502_CORE_DIR}/def/keyboard_*.def)

# rp6502_gen_kbdlay(<target>) -> KBDLAY_C, KBDLAY_H, KBDLAY_DIR
#
# The layouts as one image core/hid/layout.c reads a word at a time.
function(rp6502_gen_kbdlay target)
    set(gen ${RP6502_CORE_DIR}/gen/keyboard_layout_gen.py)
    set(c ${CMAKE_CURRENT_BINARY_DIR}/kbdlay.c)
    set(h ${CMAKE_CURRENT_BINARY_DIR}/kbdlay.h)
    add_custom_command(OUTPUT ${c} ${h}
        COMMAND ${CMAKE_COMMAND} -E env python3 ${gen}
            --manifest ${KBDLAY_MANIFEST} --emit-c ${c} --emit-h ${h}
        DEPENDS ${gen} ${KBDLAY_MANIFEST} ${KBDLAY_DEFS}
        COMMENT "Generating the keyboard layouts"
        VERBATIM)
    add_custom_target(${target} DEPENDS ${c} ${h})
    set(KBDLAY_C ${c} PARENT_SCOPE)
    set(KBDLAY_H ${h} PARENT_SCOPE)
    set(KBDLAY_DIR ${CMAKE_CURRENT_BINARY_DIR} PARENT_SCOPE)
endfunction()
