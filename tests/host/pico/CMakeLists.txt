# msc.c with FatFs, built for the host against TinyUSB's headers, which need a
# GNU-compatible compiler.
if(NOT MSVC)
    rp6502_submodule(vendor/tinyusb
        SENTINEL src/portable/raspberrypi/rp2040/rp2040_usb.c
        RESULT _tinyusb OPTIONAL WANTS "the USB floppy tests")
    if(_tinyusb)
        rp6502_add_test(msc
            SOURCES test_msc.c
                ${RP6502_SRC}/host/pico/ria/usb/msc.c
                ${RP6502_VENDOR}/fatfs/ff.c
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}/stub
                ${RP6502_SRC}/host/pico/ria ${RP6502_SRC}/host/pico
                ${RP6502_VENDOR} ${RP6502_VENDOR}/tinyusb/src
            DEFS CFG_TUSB_MCU=OPT_MCU_NONE CFG_TUSB_OS=OPT_OS_NONE
                RP6502_EXFAT=0 PICO_PROGRAM_NAME="RP6502-RIA" RP6502_LOCALE=EN)
        rp6502_add_test(vcp
            SOURCES test_vcp.c ${RP6502_SRC}/host/pico/ria/usb/vcp.c
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}/vcp ${CMAKE_CURRENT_LIST_DIR}/stub
                ${RP6502_SRC}/host/pico/ria ${RP6502_SRC}/host/pico
                ${RP6502_VENDOR} ${RP6502_VENDOR}/tinyusb/src
            DEFS CFG_TUSB_MCU=OPT_MCU_NONE CFG_TUSB_OS=OPT_OS_NONE
                RP6502_EXFAT=0 PICO_PROGRAM_NAME="RP6502-RIA" RP6502_LOCALE=EN)
    endif()
endif()

# The NDEF parsing of the NFC reader, which needs nothing of the Pico.
set_source_files_properties(${RP6502_GEN}/unicode_oem.c PROPERTIES GENERATED TRUE)
rp6502_add_test(ndef
    SOURCES test_ndef.c ${RP6502_SRC}/host/pico/ria/usb/ndef.c
        ${RP6502_SRC}/core/str/unicode.c ${RP6502_GEN}/unicode_oem.c
    INCLUDES ${RP6502_SRC}/host/pico ${RP6502_SRC}/host/pocket ${RP6502_GEN}
        ${RP6502_VENDOR})
add_dependencies(test_ndef rp6502_gen_c)

# path.c with FatFs on a disk in RAM.
rp6502_add_test(path
    SOURCES test_path.c ${RP6502_SRC}/host/pico/ria/sys/path.c
        ${RP6502_VENDOR}/fatfs/ff.c ${RP6502_VENDOR}/fatfs/ffunicode.c
    INCLUDES ${CMAKE_CURRENT_LIST_DIR}/stub ${RP6502_SRC}/host/pico/ria
        ${RP6502_SRC}/host/pico ${RP6502_VENDOR}
    DEFS RP6502_EXFAT=0 PICO_PROGRAM_NAME="RP6502-RIA" RP6502_LOCALE=EN)
