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

# The RIA's terminal, which follows the VGA's in text cells, built as the
# RIA's machine.h builds it.
rp6502_add_test(ria_cells
    SOURCES test_cells.c
        ${RP6502_SRC}/core/term/term.c ${RP6502_SRC}/core/term/grid.c
        ${RP6502_SRC}/core/term/screen.c ${RP6502_SRC}/core/term/vterm.c
        ${RP6502_SRC}/core/term/color.c ${RP6502_SRC}/core/term/cell_text.c
        ${RP6502_SRC}/core/aud/bel.c ${RP6502_ROOT}/tests/bench/tb_version.c
    INCLUDES ${CMAKE_CURRENT_LIST_DIR}/stub ${RP6502_SRC}/host/pico/ria
        ${RP6502_SRC}/host/pico ${RP6502_SRC} ${RP6502_GEN}
    DEFS PICO_PROGRAM_NAME="RP6502-RIA" RP6502_LOCALE=EN
        TERM_MAX_HEIGHT=32 TERM_ALT_SCREEN=1 TERM_CELL_TEXT=1)
add_dependencies(test_ria_cells rp6502_gen_c)

# The Pico VGA's console screen, a job done 11 rows a pass and mode 0's
# words written once a frame at most, built as the VGA's machine.h builds it.
rp6502_add_test(vga_draw
    SOURCES test_draw.c
        ${RP6502_SRC}/core/term/draw.c ${RP6502_SRC}/core/term/term.c
        ${RP6502_SRC}/core/term/grid.c ${RP6502_SRC}/core/term/screen.c
        ${RP6502_SRC}/core/term/vterm.c ${RP6502_SRC}/core/term/color.c
        ${RP6502_SRC}/core/term/cell_mode0.c
    INCLUDES ${CMAKE_CURRENT_LIST_DIR}/stub ${RP6502_GEN}
    DEFS TERM_MAX_HEIGHT=32 TERM_ALT_SCREEN=1 TERM_JOB_ROWS=11 DRAW_PUBLISH_PER_FRAME=1)
add_dependencies(test_vga_draw rp6502_gen_c)

# A paste of the machine's own text, through the RIA's keyboard row
# (KEYMAP_COM_SOURCE) and its paste stage, keys typed through keymap.c and
# its chords through key.c, built as the RIA's machine.h builds it.
rp6502_add_test(ria_paste
    SOURCES test_paste.c
        ${RP6502_SRC}/core/hid/paste.c ${RP6502_SRC}/core/hid/clip_byte.c
        ${RP6502_SRC}/core/hid/typed.c ${RP6502_SRC}/core/hid/keymap.c
        ${RP6502_SRC}/core/hid/key.c ${RP6502_SRC}/core/hid/vtkey.c
        ${RP6502_SRC}/core/sys/ring.c
        ${RP6502_SRC}/core/term/term.c ${RP6502_SRC}/core/term/grid.c
        ${RP6502_SRC}/core/term/screen.c ${RP6502_SRC}/core/term/vterm.c
        ${RP6502_SRC}/core/term/color.c ${RP6502_SRC}/core/term/cell.c
        ${RP6502_SRC}/core/term/cell_text.c ${RP6502_SRC}/core/aud/bel.c
        ${RP6502_SRC}/core/str/unicode.c ${RP6502_GEN}/unicode_oem.c
        ${RP6502_ROOT}/tests/bench/tb_version.c
    INCLUDES ${CMAKE_CURRENT_LIST_DIR}/stub ${RP6502_SRC}/host/pico/ria
        ${RP6502_SRC}/host/pico ${RP6502_SRC} ${RP6502_GEN}
    DEFS PICO_PROGRAM_NAME="RP6502-RIA" RP6502_LOCALE=EN
        TERM_MAX_HEIGHT=32 TERM_ALT_SCREEN=1 TERM_CELL_TEXT=1 TYPED_QUEUE_SIZE=32)
add_dependencies(test_ria_paste rp6502_gen_c)

# The messages the VGA answers and a 6502 program's xreg burst, vga.c and
# pix.c as the RIA builds them, against a fake VGA on hardware stubbed in vga/.
# Each case boots a RIA of its own, in a process of its own. pix.c's
# fallthrough attribute needs a GNU-compatible compiler.
if(NOT MSVC)
    rp6502_add_test(ria_vga SPLIT
        SOURCES test_vga.c
            ${RP6502_SRC}/host/pico/ria/sys/vga.c ${RP6502_SRC}/host/pico/ria/sys/pix.c
        INCLUDES ${CMAKE_CURRENT_LIST_DIR}/vga ${CMAKE_CURRENT_LIST_DIR}/stub
            ${RP6502_SRC}/host/pico/ria ${RP6502_SRC}/host/pico ${RP6502_SRC} ${RP6502_GEN}
        DEFS PICO_PROGRAM_NAME="RP6502-RIA" RP6502_LOCALE=EN
            TERM_MAX_HEIGHT=32 TERM_ALT_SCREEN=1 TERM_CELL_TEXT=1)
    add_dependencies(test_ria_vga rp6502_gen_c)
endif()
