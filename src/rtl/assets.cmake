# The generated assets: decode tables, font and palette images, sine and
# audio ROMs, code pages, and the .rp6502 programs the bench loads. All of
# them come out of src/gen, none of them is committed, and several concerns
# need the same ones — test_font reads the font tables, the verilated machine
# compiles the palette and sine packages in, and the Pocket's dist tree ships
# the font image. So they are built once here rather than by whoever asked
# first.
#
# Included by machine.cmake, which every configuration of this tree includes. The
# emulator's tree needs none of this.

set(RP6502_ASSETS ${CMAKE_BINARY_DIR}/assets)
file(MAKE_DIRECTORY ${RP6502_ASSETS})

# rp6502_asset(<target> GEN <script> OUTPUTS <file>... [ARGS <arg>...]
#              [DEPENDS <file>...] COMMENT <text>)
#
# Generates at configure time AND at build time, which looks redundant and is
# not: verilate() reads its sources when cmake configures, so a package that
# only appears at build time is missing when the model is elaborated. The
# build-time rule is what keeps it fresh when the generator changes.
#
# Existence is the whole of what configure needs, so it generates only when
# something is missing. Running unconditionally moved every asset's timestamp
# on every configure, and the extension configures on every edit to a
# CMakeLists — which put a fresh mtime on five packages the bitstream is fitted
# from, so a comment reworded here read downstream as a design that had
# changed. That costs a ten minute refit, and it makes the fit's own freshness
# gate refuse a fit nothing had actually touched.
function(rp6502_asset target)
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

# Asked for here and not inside rp6502_asset, which only generates when an
# output is missing: a warm tree whose submodule went away would otherwise
# configure clean and fail at the build rule instead.
include(${RP6502_ROOT}/rp6502_submodule.cmake)
rp6502_submodule(vendor/chips SENTINEL codegen/w65c02_gen.py
    WANTS "the cpu65 decode table generator")
rp6502_submodule(vendor/opl2_fpga
    SENTINEL fpga/modules/operator/src/opl2_log_sine_lut.sv
    WANTS "the OPL2 core and its lookup tables")

# --- The generator agrees with the C it generates from ---
# cpu65's decode tables come from vendor/chips_rp6502, so an upstream change to
# the addressing or the cycle sequences has to be modelled here before it can
# reach the RTL. The generator fails on anything it does not recognise.
set(CPU65_GEN ${RP6502_SRC}/gen/cpu65_gen.py)
add_test(NAME cpu65_gen
    COMMAND ${CMAKE_COMMAND} -E env python3 ${CPU65_GEN} --report)

# The bitstream byte-reversal for the Pocket's rbf_r, an involution.
add_test(NAME rbf_r
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_SRC}/gen/rbf_r_gen.py --check)

# ria/hid/kbd.c is compiled for a machine with USB and for one without,
# and the one without gets a shim rather than a USB stack. Two spellings
# of one specification drift; this is what says they have not.
add_test(NAME hid_shim
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_SRC}/gen/hid_shim_check.py
        --shim ${RP6502_SRC}/rtl/sw/shim/class/hid/hid.h
        --vendor ${RP6502_VENDOR}/tinyusb/src/class/hid/hid.h)

set(CPU65_ROM ${RP6502_ASSETS}/cpu65_rom_pkg.sv)
rp6502_asset(cpu65_rom GEN ${CPU65_GEN}
    ARGS --emit ${CPU65_ROM}
    OUTPUTS ${CPU65_ROM}
    COMMENT "Generating the cpu65 decode table")

# The font asset comes from vga/term/font.c: the image the firmware
# copies into the store, an offsets header for it, and the tables
# test_font checks byte for byte against font_init's runtime image.
set(VID_FONT_BIN ${RP6502_ASSETS}/fonts.bin)
set(VID_FONT_H ${RP6502_ASSETS}/vid_font_tables.h)
set(VID_FONT_ASSET_H ${RP6502_ASSETS}/vid_font_asset.h)
rp6502_asset(vid_font_rom GEN ${RP6502_SRC}/gen/vid_font_gen.py
    ARGS --emit-bin ${VID_FONT_BIN} --emit-h ${VID_FONT_H}
        --emit-asset-h ${VID_FONT_ASSET_H}
    OUTPUTS ${VID_FONT_BIN} ${VID_FONT_H} ${VID_FONT_ASSET_H}
    DEPENDS ${RP6502_SRC}/vga/term/font.c
    COMMENT "Generating the font asset")

# The builtin palettes ride the same way, from vga/term/color.c.
set(VID_PALETTE_PKG ${RP6502_ASSETS}/vid_palette_pkg.sv)
set(VID_PALETTE_H ${RP6502_ASSETS}/vid_palette_tables.h)
rp6502_asset(vid_palette_rom GEN ${RP6502_SRC}/gen/vid_palette_gen.py
    ARGS --emit-sv ${VID_PALETTE_PKG} --emit-h ${VID_PALETTE_H}
    OUTPUTS ${VID_PALETTE_PKG} ${VID_PALETTE_H}
    DEPENDS ${RP6502_SRC}/vga/term/color.c
    COMMENT "Generating the vid palette ROM")

# The PSG's sine table, from aud_init's runtime formula.
set(AUD_SINE_PKG ${RP6502_ASSETS}/aud_sine_pkg.sv)
set(AUD_SINE_H ${RP6502_ASSETS}/aud_sine_tables.h)
rp6502_asset(aud_sine_rom GEN ${RP6502_SRC}/gen/aud_sine_gen.py
    ARGS --emit-sv ${AUD_SINE_PKG} --emit-h ${AUD_SINE_H}
    OUTPUTS ${AUD_SINE_PKG} ${AUD_SINE_H}
    COMMENT "Generating the aud sine ROM")

# The OPL2's two operator tables as one 512x12 package. The vendor's
# generated case arms are the source — parsed and re-emitted, so the
# fabric's words are the submodule's by construction — and the formulas
# from its headers are recomputed as a tripwire against a submodule
# update changing either table silently.
set(OPL2_LUT_SRC ${RP6502_VENDOR}/opl2_fpga/fpga/modules/operator/src)
set(OPL2_LUT_PKG ${RP6502_ASSETS}/opl2_lut_pkg.sv)
set(OPL2_LUT_H ${RP6502_ASSETS}/opl2_lut_tables.h)
rp6502_asset(opl2_lut_rom GEN ${RP6502_SRC}/gen/opl2_lut_gen.py
    ARGS --log-sine ${OPL2_LUT_SRC}/opl2_log_sine_lut.sv
        --exp ${OPL2_LUT_SRC}/opl2_exp_lut.sv
        --emit-sv ${OPL2_LUT_PKG} --emit-h ${OPL2_LUT_H}
    OUTPUTS ${OPL2_LUT_PKG} ${OPL2_LUT_H}
    DEPENDS ${OPL2_LUT_SRC}/opl2_log_sine_lut.sv
        ${OPL2_LUT_SRC}/opl2_exp_lut.sv
    COMMENT "Generating the merged OPL2 LUT ROM")

# Two audio programs that make one note and leave the console alone, so
# the machine's own diagnostics stay readable while a device is driven.
# test_aud runs these same files, which is what keeps a note that sounds
# on hardware and a note the simulation asserts from drifting apart.
# The assembler and the .rp6502 container every one of these generators
# writes through. A change to it changes every ROM, so every ROM names it.
set(RP6502_ROM_GEN ${RP6502_SRC}/gen/rp6502_rom.py)

set(AUD_ROM_PSG ${RP6502_ASSETS}/psg.rp6502)
set(AUD_ROM_PSG_PRE ${RP6502_ASSETS}/psg_pre.rp6502)
set(AUD_ROM_OPL ${RP6502_ASSETS}/opl.rp6502)
set(AUD_ROM_OPL_EXIT ${RP6502_ASSETS}/opl_exit.rp6502)
set(AUD_ROM_BEL ${RP6502_ASSETS}/bel.rp6502)
set(AUD_ROM_OPL_BEL ${RP6502_ASSETS}/opl_bel.rp6502)
rp6502_asset(aud_roms GEN ${RP6502_SRC}/gen/aud_rom_gen.py
    ARGS --emit-psg ${AUD_ROM_PSG} --emit-psg-pre ${AUD_ROM_PSG_PRE}
        --emit-opl ${AUD_ROM_OPL}
        --emit-opl-exit ${AUD_ROM_OPL_EXIT}
        --emit-bel ${AUD_ROM_BEL} --emit-opl-bel ${AUD_ROM_OPL_BEL}
    OUTPUTS ${AUD_ROM_PSG} ${AUD_ROM_PSG_PRE} ${AUD_ROM_OPL}
        ${AUD_ROM_OPL_EXIT}
        ${AUD_ROM_BEL} ${AUD_ROM_OPL_BEL}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the audio bring-up ROMs")

# The resampler's coefficients, as the package the RTL reads. The same
# script writes the C table in src/emu, so there is one design behind both
# and the lockstep is comparing implementations rather than designs.
set(RSMP_COEF_PKG ${RP6502_ASSETS}/rsmp_coef_pkg.sv)
rp6502_asset(rsmp_coef_pkg GEN ${RP6502_SRC}/gen/rsmp_coef_gen.py
    ARGS --emit-sv ${RSMP_COEF_PKG}
    OUTPUTS ${RSMP_COEF_PKG}
    COMMENT "Generating the resampler coefficient package")

# The OEM code page tables. This machine cannot link them in, so it gets
# the binary and loads it into the staging store beside the fonts.
set(OEMCP_SRC ${RP6502_VENDOR}/fatfs/ffunicode.c)
set(OEMCP_BIN ${RP6502_ASSETS}/oemcp.bin)
rp6502_asset(oemcp_bin GEN ${RP6502_SRC}/gen/oem_table_gen.py
    ARGS --ffunicode ${OEMCP_SRC} --emit-bin ${OEMCP_BIN}
    OUTPUTS ${OEMCP_BIN}
    DEPENDS ${OEMCP_SRC}
    COMMENT "Generating the OEM code page tables")

# The keyboard layouts, for the same reason: twenty kilobytes of table
# as a compiler lays it out, eight as the generator does, and no room
# for either in a 96 KB tightly coupled memory.
set(KBDLAY_MANIFEST ${RP6502_SRC}/ria/def/kbd.def)
file(GLOB KBDLAY_DEFS ${RP6502_SRC}/ria/def/kbd_*.def)
set(KBDLAY_BIN ${RP6502_ASSETS}/kbdlay.bin)
rp6502_asset(kbdlay_bin GEN ${RP6502_SRC}/gen/kbd_layout_gen.py
    ARGS --manifest ${KBDLAY_MANIFEST} --emit-bin ${KBDLAY_BIN}
    OUTPUTS ${KBDLAY_BIN}
    DEPENDS ${KBDLAY_MANIFEST} ${KBDLAY_DEFS}
    COMMENT "Generating the keyboard layouts")

# The menu picks a layout by its position in the manifest and the data
# slot declares the image's exact size, so both are checked against
# def/kbd.def rather than kept in step by hand.
set(POCKET_CORE_JSON
    ${RP6502_SRC}/dist/pocket/Cores/Rumbledethumps.RP6502)
add_test(NAME kbdlay_json
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_SRC}/gen/kbd_layout_gen.py --manifest ${KBDLAY_MANIFEST}
        --check-interact ${POCKET_CORE_JSON}/interact.json
        --check-data ${POCKET_CORE_JSON}/data.json)

# The file round trip, generated the same way and shipped the same way.
set(FILE_ROM ${RP6502_ASSETS}/file.rp6502)
rp6502_asset(file_rom GEN ${RP6502_SRC}/gen/file_rom_gen.py
    ARGS --emit ${FILE_ROM}
    OUTPUTS ${FILE_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the file round-trip ROM")

# The same round trip past the transfer window. It ships but is not a
# test: what it exists to ask — whether the Pocket's resize keeps what
# was already in the file — has no answer in simulation, because the
# bench answers the way we assumed.
set(BIGFILE_ROM ${RP6502_ASSETS}/bigfile.rp6502)
rp6502_asset(bigfile_rom GEN ${RP6502_SRC}/gen/bigfile_rom_gen.py
    ARGS --emit ${BIGFILE_ROM}
    OUTPUTS ${BIGFILE_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the multi-chunk file ROM")

# The create path has never worked on hardware and the name turned out
# not to matter. This walks a list of names in one boot so the next
# guess costs a card copy instead of a fit.
set(PROBE_ROM ${RP6502_ASSETS}/probe.rp6502)
rp6502_asset(probe_rom GEN ${RP6502_SRC}/gen/probe_rom_gen.py
    ARGS --emit ${PROBE_ROM}
    OUTPUTS ${PROBE_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the open-file probe ROM")

# The whole drive in one boot: forty-seven checks the machine decides
# for itself. It runs here against the bench's host as well as on the
# card, so a bug in the ROM is found before a photograph is.
set(FSTEST_ROM ${RP6502_ASSETS}/fstest.rp6502)
rp6502_asset(fstest_rom GEN ${RP6502_SRC}/gen/fstest_rom_gen.py
    ARGS --emit ${FSTEST_ROM}
    OUTPUTS ${FSTEST_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the filesystem conformance ROM")
