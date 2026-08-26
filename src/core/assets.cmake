# The generated assets: decode tables, font and palette images, sine and
# audio lookup ROMs, code pages. All of them come out of src/core/gen, none of them
# is committed, and several concerns need the same ones — test_font reads the
# font tables, the verilated machine compiles the palette and sine packages in,
# and the Pocket's dist tree ships the font image. So they are built once here
# rather than by whoever asked first.
#
# What is here is what the machine is built from or staged into. A .rp6502 is
# neither: it is a program the machine runs, and the same one runs on all
# three, so those are made in tests/rp6502_tests.cmake where both trees can
# reach them. They were here once, and this tree was the only one that could
# build them.
#
# Included by machine.cmake, which every configuration of this tree includes. The
# emulator's tree needs none of this.

set(RP6502_ASSETS ${CMAKE_BINARY_DIR}/assets)
file(MAKE_DIRECTORY ${RP6502_ASSETS})

# rp6502_machine_asset(<target> GEN <script> OUTPUTS <file>... [ARGS <arg>...]
#                      [DEPENDS <file>...] COMMENT <text>)
#
# Named for what it makes: something the machine is built from or staged into.
# rp6502_asset is the SDK's, in tools/rp6502.cmake, and takes an address and a
# file — a downstream project calls that one, and the two shared a name.
#
# Generates at configure time AND at build time, which looks redundant and is
# not: verilate() reads its sources when cmake configures, so a package that
# only appears at build time is missing when the model is elaborated. The
# build-time rule is what keeps it fresh when the generator changes.
#
# Existence is the whole of what configure needs, so it generates only when
# something is missing. Running unconditionally moves every asset's timestamp
# on every configure, and the IDE configures on every CMakeLists edit — which
# reads downstream as a changed design and costs a ten minute refit.
function(rp6502_machine_asset target)
    cmake_parse_arguments(A "" "GEN;COMMENT" "OUTPUTS;ARGS;DEPENDS" ${ARGN})
    set(_absent FALSE)
    foreach(_out IN LISTS A_OUTPUTS)
        if(NOT EXISTS ${_out})
            set(_absent TRUE)
        endif()
    endforeach()
    if(_absent)
        execute_process(COMMAND python3 ${A_GEN} ${A_ARGS} RESULT_VARIABLE _rc)
        if(_rc)
            get_filename_component(_name ${A_GEN} NAME)
            message(FATAL_ERROR "${_name} failed")
        endif()
    endif()
    add_custom_command(OUTPUT ${A_OUTPUTS}
        COMMAND ${CMAKE_COMMAND} -E env python3 ${A_GEN} ${A_ARGS}
        DEPENDS ${A_GEN} ${A_DEPENDS}
        COMMENT ${A_COMMENT}
        VERBATIM)
    add_custom_target(${target} ALL DEPENDS ${A_OUTPUTS})
endfunction()

# Asked for here and not inside rp6502_machine_asset, which only generates when
# an output is missing: a warm tree whose submodule went away would otherwise
# configure clean and fail at the build rule instead.
include(${RP6502_ROOT}/submodules.cmake)
rp6502_submodule(vendor/chips SENTINEL codegen/w65c02_gen.py
    WANTS "the w65c02 decode table generator")
rp6502_submodule(vendor/opl2_fpga
    SENTINEL fpga/modules/operator/src/opl2_log_sine_lut.sv
    WANTS "the OPL2 core and its lookup tables")

# --- The generator agrees with the C it generates from ---
# w65c02's decode tables come from vendor/chips, so an upstream change to
# the addressing or the cycle sequences has to be modelled here before it can
# reach the RTL. The generator fails on anything it does not recognise.
set(W65C02_GEN ${RP6502_SRC}/core/gen/w65c02_rom_gen.py)
add_test(NAME w65c02_gen
    COMMAND ${CMAKE_COMMAND} -E env python3 ${W65C02_GEN} --report)
set_tests_properties(w65c02_gen PROPERTIES LABELS "gate")

# The bitstream byte-reversal for the Pocket's rbf_r, an involution.
add_test(NAME rbf_r
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_SRC}/core/gen/rbf_r_gen.py --check)
set_tests_properties(rbf_r PROPERTIES LABELS "gate")

set(W65C02_ROM ${RP6502_ASSETS}/w65c02_rom_pkg.sv)
rp6502_machine_asset(w65c02_rom GEN ${W65C02_GEN}
    ARGS --emit ${W65C02_ROM}
    OUTPUTS ${W65C02_ROM}
    COMMENT "Generating the w65c02 decode table")

# The font asset comes from core/term/font.c: the image the firmware
# copies into the store, and an offsets header for it.
set(VID_FONT_BIN ${RP6502_ASSETS}/fonts.bin)
set(VID_FONT_ASSET_H ${RP6502_ASSETS}/vid_font_asset.h)
rp6502_machine_asset(vid_font_rom GEN ${RP6502_SRC}/core/gen/vid_font_gen.py
    ARGS --emit-bin ${VID_FONT_BIN} --emit-asset-h ${VID_FONT_ASSET_H}
    OUTPUTS ${VID_FONT_BIN} ${VID_FONT_ASSET_H}
    DEPENDS ${RP6502_SRC}/core/term/font.c
    COMMENT "Generating the font asset")

# The builtin palettes ride the same way, from core/term/color.c.
set(VID_PALETTE_PKG ${RP6502_ASSETS}/vid_palette_pkg.sv)
rp6502_machine_asset(vid_palette_rom GEN ${RP6502_SRC}/core/gen/vid_palette_gen.py
    ARGS --emit-sv ${VID_PALETTE_PKG}
    OUTPUTS ${VID_PALETTE_PKG}
    DEPENDS ${RP6502_SRC}/core/term/color.c
    COMMENT "Generating the vid palette ROM")

# The PSG's sine table, from aud_init's runtime formula.
set(AUD_SINE_PKG ${RP6502_ASSETS}/aud_sine_pkg.sv)
rp6502_machine_asset(aud_sine_rom GEN ${RP6502_SRC}/core/gen/aud_sine_gen.py
    ARGS --emit-sv ${AUD_SINE_PKG}
    OUTPUTS ${AUD_SINE_PKG}
    COMMENT "Generating the aud sine ROM")

# The OPL2's two operator tables as one 512x12 package. The vendor's
# generated case arms are the source — parsed and re-emitted, so the
# fabric's words are the submodule's by construction — and the formulas
# from its headers are recomputed as a tripwire against a submodule
# update changing either table silently.
set(OPL2_LUT_SRC ${RP6502_VENDOR}/opl2_fpga/fpga/modules/operator/src)
set(OPL2_LUT_PKG ${RP6502_ASSETS}/opl2_lut_pkg.sv)
rp6502_machine_asset(opl2_lut_rom GEN ${RP6502_SRC}/core/gen/opl2_lut_gen.py
    ARGS --log-sine ${OPL2_LUT_SRC}/opl2_log_sine_lut.sv
        --exp ${OPL2_LUT_SRC}/opl2_exp_lut.sv
        --emit-sv ${OPL2_LUT_PKG}
    OUTPUTS ${OPL2_LUT_PKG}
    DEPENDS ${OPL2_LUT_SRC}/opl2_log_sine_lut.sv
        ${OPL2_LUT_SRC}/opl2_exp_lut.sv
    COMMENT "Generating the merged OPL2 LUT ROM")


# The resampler's coefficients, as the package the RTL reads. The same
# script writes the C table in src/core/emu, so there is one design behind both
# and the lockstep is comparing implementations rather than designs.
set(RSMP_COEF_PKG ${RP6502_ASSETS}/rsmp_coef_pkg.sv)
rp6502_machine_asset(rsmp_coef_pkg GEN ${RP6502_SRC}/core/gen/rsmp_coef_gen.py
    ARGS --emit-sv ${RSMP_COEF_PKG}
    OUTPUTS ${RSMP_COEF_PKG}
    COMMENT "Generating the resampler coefficient package")

# The OEM code page tables. This machine cannot link them in, so it gets
# the binary and loads it into the staging store beside the fonts.
set(OEMCP_SRC ${RP6502_VENDOR}/fatfs/ffunicode.c)
set(OEMCP_BIN ${RP6502_ASSETS}/oemcp.bin)
rp6502_machine_asset(oemcp_bin GEN ${RP6502_SRC}/core/gen/oem_table_gen.py
    ARGS --ffunicode ${OEMCP_SRC} --emit-bin ${OEMCP_BIN}
    OUTPUTS ${OEMCP_BIN}
    DEPENDS ${OEMCP_SRC}
    COMMENT "Generating the OEM code page tables")

# The keyboard layouts, for the same reason: twenty kilobytes of table
# as a compiler lays it out, eight as the generator does, and no room
# for either in a 96 KB tightly coupled memory.
set(KBDLAY_MANIFEST ${RP6502_SRC}/core/def/keyboard.def)
file(GLOB KBDLAY_DEFS ${RP6502_SRC}/core/def/keyboard_*.def)
set(KBDLAY_BIN ${RP6502_ASSETS}/keyboard.bin)
rp6502_machine_asset(kbdlay_bin GEN ${RP6502_SRC}/core/gen/keyboard_layout_gen.py
    ARGS --manifest ${KBDLAY_MANIFEST} --emit-bin ${KBDLAY_BIN}
    OUTPUTS ${KBDLAY_BIN}
    DEPENDS ${KBDLAY_MANIFEST} ${KBDLAY_DEFS}
    COMMENT "Generating the keyboard layouts")

# The menu picks a layout by its position in the manifest and the data
# slot declares the image's exact size, so both are checked against
# def/keyboard.def rather than kept in step by hand.
set(POCKET_CORE_JSON
    ${RP6502_SRC}/mach/pocket/dist/Cores/Rumbledethumps.RP6502)
add_test(NAME kbdlay_json
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_SRC}/core/gen/keyboard_layout_gen.py --manifest ${KBDLAY_MANIFEST}
        --check-interact ${POCKET_CORE_JSON}/interact.json
        --check-data ${POCKET_CORE_JSON}/data.json)
set_tests_properties(kbdlay_json PROPERTIES LABELS "gate")

# Where the host writes each slot and where the firmware reads it are
# the same map kept in two files, and a disagreement is silent.
add_test(NAME stage_map
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_SRC}/core/gen/stage_map_gate.py
        --data ${POCKET_CORE_JSON}/data.json
        --mmio ${RP6502_SRC}/host/pocket/sw/mmio.h
        --bench ${RP6502_ROOT}/tests/bench/tb_stage.h
        --engine ${RP6502_SRC}/core/machine/sst_engine.sv
        --sst ${RP6502_HOST_POCKET}/pocket_sst.sv
        --top ${RP6502_SRC}/host/pocket/core_top.sv)
set_tests_properties(stage_map PROPERTIES LABELS "gate")
