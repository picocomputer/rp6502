set(RP6502_ASSETS ${CMAKE_BINARY_DIR}/assets)
file(MAKE_DIRECTORY ${RP6502_ASSETS})

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

include(${RP6502_ROOT}/rp6502_submodule.cmake)
rp6502_submodule(vendor/chips SENTINEL codegen/w65c02_gen.py
    WANTS "the cpu65 decode table generator")
rp6502_submodule(vendor/opl2_fpga
    SENTINEL fpga/modules/operator/src/opl2_log_sine_lut.sv
    WANTS "the OPL2 core and its lookup tables")

set(CPU65_GEN ${RP6502_SRC}/gen/cpu65_gen.py)
add_test(NAME cpu65_gen
    COMMAND ${CMAKE_COMMAND} -E env python3 ${CPU65_GEN} --report)

add_test(NAME rbf_r
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_SRC}/gen/rbf_r_gen.py --check)

set(CPU65_ROM ${RP6502_ASSETS}/cpu65_rom_pkg.sv)
rp6502_asset(cpu65_rom GEN ${CPU65_GEN}
    ARGS --emit ${CPU65_ROM}
    OUTPUTS ${CPU65_ROM}
    COMMENT "Generating the cpu65 decode table")

set(VID_FONT_BIN ${RP6502_ASSETS}/fonts.bin)
set(VID_FONT_H ${RP6502_ASSETS}/vid_font_tables.h)
set(VID_FONT_ASSET_H ${RP6502_ASSETS}/vid_font_asset.h)
rp6502_asset(vid_font_rom GEN ${RP6502_SRC}/gen/vid_font_gen.py
    ARGS --emit-bin ${VID_FONT_BIN} --emit-h ${VID_FONT_H}
        --emit-asset-h ${VID_FONT_ASSET_H}
    OUTPUTS ${VID_FONT_BIN} ${VID_FONT_H} ${VID_FONT_ASSET_H}
    DEPENDS ${RP6502_SRC}/vga/term/font.c
    COMMENT "Generating the font asset")

set(VID_PALETTE_PKG ${RP6502_ASSETS}/vid_palette_pkg.sv)
set(VID_PALETTE_H ${RP6502_ASSETS}/vid_palette_tables.h)
rp6502_asset(vid_palette_rom GEN ${RP6502_SRC}/gen/vid_palette_gen.py
    ARGS --emit-sv ${VID_PALETTE_PKG} --emit-h ${VID_PALETTE_H}
    OUTPUTS ${VID_PALETTE_PKG} ${VID_PALETTE_H}
    DEPENDS ${RP6502_SRC}/vga/term/color.c
    COMMENT "Generating the vid palette ROM")

set(AUD_SINE_PKG ${RP6502_ASSETS}/aud_sine_pkg.sv)
set(AUD_SINE_H ${RP6502_ASSETS}/aud_sine_tables.h)
rp6502_asset(aud_sine_rom GEN ${RP6502_SRC}/gen/aud_sine_gen.py
    ARGS --emit-sv ${AUD_SINE_PKG} --emit-h ${AUD_SINE_H}
    OUTPUTS ${AUD_SINE_PKG} ${AUD_SINE_H}
    COMMENT "Generating the aud sine ROM")

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

set(RSMP_COEF_PKG ${RP6502_ASSETS}/rsmp_coef_pkg.sv)
rp6502_asset(rsmp_coef_pkg GEN ${RP6502_SRC}/gen/rsmp_coef_gen.py
    ARGS --emit-sv ${RSMP_COEF_PKG}
    OUTPUTS ${RSMP_COEF_PKG}
    COMMENT "Generating the resampler coefficient package")

set(OEMCP_SRC ${RP6502_VENDOR}/fatfs/ffunicode.c)
set(OEMCP_BIN ${RP6502_ASSETS}/oemcp.bin)
rp6502_asset(oemcp_bin GEN ${RP6502_SRC}/gen/oem_table_gen.py
    ARGS --ffunicode ${OEMCP_SRC} --emit-bin ${OEMCP_BIN}
    OUTPUTS ${OEMCP_BIN}
    DEPENDS ${OEMCP_SRC}
    COMMENT "Generating the OEM code page tables")

set(KBDLAY_MANIFEST ${RP6502_SRC}/ria/def/kbd.def)
file(GLOB KBDLAY_DEFS ${RP6502_SRC}/ria/def/kbd_*.def)
set(KBDLAY_BIN ${RP6502_ASSETS}/keyboard.bin)
rp6502_asset(kbdlay_bin GEN ${RP6502_SRC}/gen/kbd_layout_gen.py
    ARGS --manifest ${KBDLAY_MANIFEST} --emit-bin ${KBDLAY_BIN}
    OUTPUTS ${KBDLAY_BIN}
    DEPENDS ${KBDLAY_MANIFEST} ${KBDLAY_DEFS}
    COMMENT "Generating the keyboard layouts")

set(POCKET_CORE_JSON
    ${RP6502_SRC}/dist/pocket/Cores/Rumbledethumps.RP6502)
add_test(NAME kbdlay_json
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_SRC}/gen/kbd_layout_gen.py --manifest ${KBDLAY_MANIFEST}
        --check-interact ${POCKET_CORE_JSON}/interact.json
        --check-data ${POCKET_CORE_JSON}/data.json)

add_test(NAME stage_map
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_SRC}/gen/stage_map_gate.py
        --data ${POCKET_CORE_JSON}/data.json
        --mmio ${RP6502_SRC}/rtl/sw/mmio.h
        --bench ${RP6502_ROOT}/tests/bench/tb_stage.h
        --engine ${RP6502_SRC}/rtl/core/sst_engine.sv
        --sst ${RP6502_HOST_POCKET}/pocket_sst.sv
        --top ${RP6502_SRC}/host/pocket/core_top.sv)

set(STREAM_ROM ${RP6502_ASSETS}/stream.rp6502)
rp6502_asset(stream_rom GEN ${RP6502_SRC}/gen/stream_rom_gen.py
    ARGS --emit ${STREAM_ROM}
    OUTPUTS ${STREAM_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the streaming-read ROM")

set(SONG_ROM ${RP6502_ASSETS}/song.rp6502)
rp6502_asset(song_rom GEN ${RP6502_SRC}/gen/song_rom_gen.py
    ARGS --emit ${SONG_ROM}
    OUTPUTS ${SONG_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the OPL sleep ROM")

set(FILE_ROM ${RP6502_ASSETS}/file.rp6502)
rp6502_asset(file_rom GEN ${RP6502_SRC}/gen/file_rom_gen.py
    ARGS --emit ${FILE_ROM}
    OUTPUTS ${FILE_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the file round-trip ROM")

set(BIGFILE_ROM ${RP6502_ASSETS}/bigfile.rp6502)
rp6502_asset(bigfile_rom GEN ${RP6502_SRC}/gen/bigfile_rom_gen.py
    ARGS --emit ${BIGFILE_ROM}
    OUTPUTS ${BIGFILE_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the multi-chunk file ROM")

set(PROBE_ROM ${RP6502_ASSETS}/probe.rp6502)
rp6502_asset(probe_rom GEN ${RP6502_SRC}/gen/probe_rom_gen.py
    ARGS --emit ${PROBE_ROM}
    OUTPUTS ${PROBE_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the open-file probe ROM")

set(FSTEST_ROM ${RP6502_ASSETS}/fstest.rp6502)
rp6502_asset(fstest_rom GEN ${RP6502_SRC}/gen/fstest_rom_gen.py
    ARGS --emit ${FSTEST_ROM}
    OUTPUTS ${FSTEST_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the filesystem conformance ROM")
