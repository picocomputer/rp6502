# The Analogue Pocket's own layer: the host bridge, its SDRAM staging, the
# scaler crossing and the codec.
#
# These are here rather than under a subsystem because they genuinely are
# about the host. A bridge FIFO crossing 74.25 MHz into 50.4 MHz is not a
# fact about the RP6502; it is a fact about this board.

set(POCKET_TB ${CMAKE_CURRENT_LIST_DIR})
set(POCKET_STACK
    ${RP6502_HOST_POCKET}/pocket_fifo.sv
    ${RP6502_HOST_POCKET}/pocket_video.sv
    ${RP6502_HOST_POCKET}/pocket_i2s.sv
    ${RP6502_HOST_POCKET}/pocket_sdram.sv
    ${RP6502_HOST_POCKET}/pocket_sram.sv
    ${RP6502_HOST_POCKET}/pocket_bridge.sv
    ${RP6502_HOST_POCKET}/pocket_file.sv
    ${RP6502_HOST_POCKET}/pocket_sst.sv
    ${RP6502_HOST_POCKET}/pocket_core.sv
    ${POCKET_TB}/sdram_model.sv
    ${POCKET_TB}/asram_model.sv
    ${POCKET_TB}/tb_pocket.sv)

# --- The platform FIFO under hostile clocking ---
rp6502_add_module_test(fifo
    TOP pocket_fifo
    RTL ${RP6502_HOST_POCKET}/pocket_fifo.sv
    TIMEOUT 120)

# --- The savestate bridge: the handshake and the outbound stream ---
rp6502_add_module_test(psst
    TOP pocket_sst
    RTL ${RP6502_HOST_POCKET}/pocket_fifo.sv
        ${RP6502_HOST_POCKET}/pocket_sst.sv
    TIMEOUT 120)

# --- The debug log, playing both the console and the host ---
rp6502_add_module_test(pdbglog
    TOP pocket_dbglog
    RTL ${RP6502_HOST_POCKET}/pocket_fifo.sv
        ${RP6502_HOST_POCKET}/pocket_dbglog.sv
    TIMEOUT 120)

# --- The scaler-side raster from a replayed beam ---
rp6502_add_module_test(pvideo
    TOP pocket_video
    RTL ${RP6502_HOST_POCKET}/pocket_fifo.sv
        ${RP6502_HOST_POCKET}/pocket_video.sv
    TIMEOUT 120)

# --- The codec-side I2S from replayed samples ---
rp6502_add_module_test(pi2s
    TOP pocket_i2s
    RTL ${RP6502_HOST_POCKET}/pocket_fifo.sv
        ${RP6502_HOST_POCKET}/pocket_i2s.sv
    TIMEOUT 120)

# --- The staging SDRAM controller against a behavioral chip ---
rp6502_add_module_test(psdram
    TOP tb_psdram
    RTL ${RP6502_HOST_POCKET}/pocket_sdram.sv
        ${POCKET_TB}/sdram_model.sv ${POCKET_TB}/tb_psdram.sv
    TIMEOUT 300)

# --- The bridge chain, played like the host ---
rp6502_add_module_test(pbridge
    TOP tb_pbridge
    RTL ${RP6502_HOST_POCKET}/pocket_fifo.sv
        ${RP6502_HOST_POCKET}/pocket_sdram.sv
        ${RP6502_HOST_POCKET}/pocket_bridge.sv
        ${POCKET_TB}/sdram_model.sv ${POCKET_TB}/tb_pbridge.sv
    TIMEOUT 300)

if(RP6502_SOFT_CPU)
    # --- The whole Pocket stack, frame-exact against the oracle ---
    rp6502_add_machine_test(pocket ORACLE FIRMWARE SPLIT
        TOP tb_pocket
        RTL ${POCKET_STACK}
        DEFS ROMS_DIR="${RP6502_TEST_CORPUS}"
        TIMEOUT 1200)

    # --- Sleep, with the reconfigure in it ---
    rp6502_add_machine_test(psleep FIRMWARE SPLIT
        TOP tb_pocket
        RTL ${POCKET_STACK}
        DEFS FILE_ROM="${FILE_ROM}" ROMS_DIR="${RP6502_TEST_CORPUS}"
        DEPENDS file_rom
        TIMEOUT 1200)

    # --- The file bridge, with the bench playing the host ---
    rp6502_add_machine_test(pfile FIRMWARE SPLIT
        TOP tb_pocket
        RTL ${POCKET_STACK}
        DEFS FILE_ROM="${FILE_ROM}" FSTEST_ROM="${FSTEST_ROM}"
        DEPENDS file_rom fstest_rom
        TIMEOUT 1200)
endif()
