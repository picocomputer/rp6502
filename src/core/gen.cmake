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
