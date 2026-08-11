if(QUARTUS_MAP AND QUARTUS_FIT AND QUARTUS_STA)
    set(SYNTH_DIR ${CMAKE_BINARY_DIR}/synth)
    set(SYNTH_QSF ${SYNTH_DIR}/rp6502.qsf)
    set(SYNTH_LINES
        "set_global_assignment -name FAMILY \"Cyclone V\""
        "set_global_assignment -name DEVICE 5CEBA4F23C8"
        "set_global_assignment -name TOP_LEVEL_ENTITY rp6502"
        "set_global_assignment -name PROJECT_OUTPUT_DIRECTORY output_files"
        "set_global_assignment -name NUM_PARALLEL_PROCESSORS ALL"
        "set_global_assignment -name SDC_FILE ${RP6502_SDC}"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl/arith"
        "set_global_assignment -name AUTO_SHIFT_REGISTER_RECOGNITION OFF"
        "set_parameter -name EXT_RAM 1"
        "set_instance_assignment -name VIRTUAL_PIN ON -to *"
        "set_global_assignment -name MESSAGE_DISABLE 15720")
    foreach(src ${RP6502_MACHINE_SOURCES})
        if(src MATCHES "\\.sv$")
            list(APPEND SYNTH_LINES
                "set_global_assignment -name SYSTEMVERILOG_FILE ${src}")
        elseif(src MATCHES "\\.v$")
            list(APPEND SYNTH_LINES
                "set_global_assignment -name VERILOG_FILE ${src}")
        endif()
    endforeach()
    string(REPLACE ";" "\n" SYNTH_QSF_TEXT "${SYNTH_LINES}")
    file(MAKE_DIRECTORY ${SYNTH_DIR})
    file(WRITE ${SYNTH_QSF} "${SYNTH_QSF_TEXT}\n")
    file(WRITE ${SYNTH_DIR}/rp6502.qpf
        "PROJECT_REVISION = \"rp6502\"\n")
    add_custom_target(synth
        COMMAND ${QUARTUS_MAP} rp6502
        COMMAND ${QUARTUS_FIT} rp6502
        COMMAND ${QUARTUS_STA} rp6502
        WORKING_DIRECTORY ${SYNTH_DIR}
        DEPENDS cpu65_rom vid_font_rom vid_palette_rom aud_sine_rom
        COMMENT "Synthesizing the machine for the Pocket's Cyclone V"
        VERBATIM)
endif()
