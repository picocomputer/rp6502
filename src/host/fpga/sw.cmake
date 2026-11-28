include_guard(GLOBAL)
include(${RP6502_SRC}/core/gen.cmake)
include(${RP6502_SRC}/core/log.cmake)
# Every host's machine.cmake has registered its generator by now.
rp6502_gen_c()
rp6502_gen_assets()

# rp6502_fpga_sw(DIR <host dir> SOURCES <host .c> DEPENDS <generated headers>)
# builds the soft CPU's firmware from src/host/fpga/sw, core and a host's own
# units.
#  - DIR is the first -I, where the host's machine.h, drivers.h and
#    fpga_host.h are found by bare name.
#  - The image is linked by src/host/fpga/sw/link.ld.
#  - RP6502_SW_LOG, when set, replaces sw/log.c.
#  - It sets SW_BIN and RP6502_SOFT_CPU in the caller's scope, makes the
#    target sw_bin and writes ${CMAKE_BINARY_DIR}/sw_units.json.
function(rp6502_fpga_sw)
    cmake_parse_arguments(PARSE_ARGV 0 F "" "DIR" "SOURCES;DEPENDS")
    find_program(RISCV_GCC riscv64-unknown-elf-gcc)
    find_program(RISCV_OBJCOPY riscv64-unknown-elf-objcopy)
    if(NOT RISCV_GCC OR NOT RISCV_OBJCOPY)
        message(WARNING
            "riscv64-unknown-elf-gcc not found — soft CPU tests skipped.\n"
            "  sudo apt-get install gcc-riscv64-unknown-elf picolibc-riscv64-unknown-elf")
        return()
    endif()
    set(_sw ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sw)
    set(_log ${RP6502_SW_LOG})
    if(NOT _log)
        set(_log ${_sw}/log.c)
    endif()
    set(SW_BIN ${RP6502_ASSETS}/sw.bin)
    file(GLOB_RECURSE SW_HEADERS CONFIGURE_DEPENDS
        ${RP6502_SRC}/core/*.h
        ${RP6502_SRC}/core/def/*.def
        ${F_DIR}/*.h
        ${_sw}/*.h)
    file(GLOB SW_SEAMS CONFIGURE_DEPENDS
        ${RP6502_SRC}/osal/*.h
        ${RP6502_SRC}/host/*.h)
    list(APPEND SW_HEADERS ${SW_SEAMS})
    set(_shared
        ${_sw}/aud.c ${_sw}/fab.c
        ${_sw}/cfg.c
        ${RP6502_SRC}/core/com/com.c ${RP6502_SRC}/core/com/manifold.c ${RP6502_SRC}/core/com/manifold_tx.c ${RP6502_SRC}/core/com/tio.c ${RP6502_SRC}/core/com/rln.c ${_sw}/ria.c ${_sw}/phi2.c ${_sw}/resb.c
        ${_sw}/mem.c ${RP6502_SRC}/core/sys/timer.c
        ${RP6502_SRC}/core/sys/crc32.c
        ${RP6502_SRC}/core/sys/sst.c
        ${_sw}/rom.c ${_sw}/time.c ${_sw}/clip.c
        ${_sw}/trap.c ${_log} ${_sw}/vga.c ${_sw}/vid.c
        ${RP6502_SRC}/core/aud/pcm.c
        ${RP6502_SRC}/core/aud/bel.c
        ${RP6502_SRC}/core/sys/pix.c
        ${RP6502_SRC}/core/rom/alias.c
        ${RP6502_SRC}/core/rom/asset.c
        ${RP6502_SRC}/core/rom/pump.c
        ${RP6502_SRC}/core/sys/buf1k.c
        ${RP6502_SRC}/core/api/xreg0.c
        ${RP6502_SRC}/core/api/xreg1.c
        ${RP6502_SRC}/core/sys/random.c
        ${RP6502_SRC}/core/sys/div64.c
        ${RP6502_SRC}/core/sys/ring.c
        ${RP6502_SRC}/core/sys/sys.c
        ${RP6502_SRC}/core/api/api.c
        ${RP6502_SRC}/core/api/arg.c
        ${RP6502_SRC}/core/api/attr.c
        ${RP6502_SRC}/core/api/proc.c
        ${RP6502_SRC}/core/api/save.c
        ${RP6502_SRC}/core/tim/clk.c
        ${RP6502_SRC}/core/tim/tim.c
        ${RP6502_SRC}/core/api/std.c
        ${RP6502_SRC}/core/com/console.c
        ${RP6502_SRC}/core/com/tty.c
        ${RP6502_SRC}/core/api/dir.c
        ${RP6502_SRC}/core/api/ops.c
        ${RP6502_SRC}/core/str/path.c
        ${RP6502_SRC}/core/str/unicode.c
        ${RP6502_GEN}/unicode_oem.c
        ${RP6502_SRC}/core/hid/hid.c
        ${RP6502_SRC}/core/hid/hostkey.c
        ${RP6502_SRC}/core/sys/event.c
        ${RP6502_SRC}/core/sys/event_pointer.c
        ${RP6502_SRC}/core/sys/event_pads.c
        ${RP6502_SRC}/core/hid/keyboard.c
        ${RP6502_SRC}/core/hid/key.c
        ${RP6502_SRC}/core/hid/layout.c
        ${RP6502_GEN}/layout_db.c
        ${RP6502_SRC}/core/hid/keymap.c
        ${RP6502_SRC}/core/hid/typed.c
        ${RP6502_SRC}/core/hid/clip_codepage.c
        ${RP6502_SRC}/core/hid/vtkey.c
        ${RP6502_SRC}/core/hid/vtmouse.c
        ${RP6502_SRC}/core/hid/mouse.c
        ${RP6502_SRC}/core/hid/gamepad.c
        ${RP6502_SRC}/core/hid/tablet.c
        ${RP6502_SRC}/core/str/str.c
        ${RP6502_SRC}/core/sys/config.c
        ${RP6502_SRC}/core/vga/canvas.c
        ${RP6502_SRC}/core/vga/mode/mode.c
        ${RP6502_SRC}/core/vga/mode/mode0.c
        ${RP6502_SRC}/core/vga/mode/mode1.c
        ${RP6502_SRC}/core/vga/mode/mode2.c
        ${RP6502_SRC}/core/vga/mode/mode3.c
        ${RP6502_SRC}/core/vga/mode/mode4.c
        ${RP6502_SRC}/core/vga/mode/mode5.c
        ${RP6502_SRC}/core/vga/mode/mode6.c
        ${RP6502_SRC}/core/term/color.c
        ${RP6502_SRC}/core/term/font.c
        ${RP6502_SRC}/core/term/term.c
        ${RP6502_SRC}/core/term/grid.c
        ${RP6502_SRC}/core/term/screen.c
        ${RP6502_SRC}/core/term/cell.c
        ${RP6502_SRC}/core/term/cell_mode0.c
        ${RP6502_SRC}/core/term/vterm.c
        ${RP6502_SRC}/core/term/vterm_mouse.c
        ${RP6502_SRC}/core/term/draw.c
        ${RP6502_SRC}/core/sys/version.c
        ${_sw}/oem.c)
    # The host's units come before the shared ones. The order steers LTO's
    # inlining, and this one makes the smaller image.
    set(SW_SOURCES ${_sw}/crt0.S ${_sw}/ver.c ${F_SOURCES} ${_shared})
    rp6502_log_flags(SW_LOG_FLAGS)
    # AUD_NATIVE_RATE is the 48 kHz the fabric mixes at, which pcm_step
    # divides by.
    set(SW_CFLAGS -march=rv32imac_zicsr_zifencei -mabi=ilp32
        -msave-restore
        -Os -ffreestanding
        --specs=picolibc.specs -DPICOLIBC_INTEGER_PRINTF_SCANF
        -ffunction-sections -fdata-sections -flto=auto
        -Werror=implicit-function-declaration
        -I ${F_DIR}
        -I ${RP6502_SRC}
        -I ${RP6502_GEN}
        -I ${RP6502_ASSETS}
        "-DPICO_PROGRAM_NAME=\"RP6502-FPGA\""
        -DRP6502_LOCALE=EN
        -DRP6502_VGA_FABRIC
        -DAUD_NATIVE_RATE=48000
        ${SW_LOG_FLAGS})
    # The firmware's version, for XTVERSION, is stamped as it is built.
    add_custom_command(OUTPUT ${SW_BIN}
        COMMAND ${CMAKE_COMMAND}
            -DOUTPUT=${RP6502_ASSETS}/rp6502_version.h
            -DSTAMP_SCRIPT=${RP6502_STAMP_SCRIPT}
            "-DSTAMP_VERSION=${RP6502_VERSION_VALUE}"
            "-DSTAMP_CI=${RP6502_CI_VALUE}"
            "-DSTAMP_GIT=${RP6502_GIT_VALUE}"
            -P ${RP6502_GEN_VERSION_SCRIPT}
        COMMAND ${RISCV_GCC} ${SW_CFLAGS}
            -nostartfiles -Wl,--gc-sections
            -T ${_sw}/link.ld
            -o ${RP6502_ASSETS}/sw.elf
            ${SW_SOURCES}
        COMMAND ${RISCV_OBJCOPY} -O binary ${RP6502_ASSETS}/sw.elf ${SW_BIN}
        DEPENDS ${SW_SOURCES} ${SW_HEADERS} ${_sw}/link.ld
            ${RP6502_GEN}/rsmp_half.h
            ${RP6502_GEN}/mode6_shapes.h
            ${F_DEPENDS}
        COMMENT "Cross-compiling the soft CPU firmware"
        VERBATIM)
    add_custom_target(sw_bin DEPENDS ${SW_BIN})

    # One command builds the whole firmware, so compile_commands.json has no
    # entry for each of its units. sw_units.json gives each one in that form,
    # for tests/sst_inventory.py, which builds every unit again. sw/log.c is
    # among them even where a test build links its own log in its place.
    set(SW_UNITS ${SW_SOURCES} ${_sw}/log.c)
    list(REMOVE_DUPLICATES SW_UNITS)
    set(_entries "")
    foreach(_src IN LISTS SW_UNITS)
        if(NOT _src MATCHES "\\.c$")
            continue()
        endif()
        set(_args "")
        foreach(_arg ${RISCV_GCC} ${SW_CFLAGS} -c ${_src})
            string(REPLACE "\\" "\\\\" _arg "${_arg}")
            string(REPLACE "\"" "\\\"" _arg "${_arg}")
            string(APPEND _args "\"${_arg}\", ")
        endforeach()
        string(APPEND _entries "  {\"directory\": \"${CMAKE_BINARY_DIR}\", "
            "\"file\": \"${_src}\", \"output\": \"sw_units/unit.o\", "
            "\"arguments\": [${_args}\"-o\", \"sw_units/unit.o\"]},\n")
    endforeach()
    string(REGEX REPLACE ",\n$" "\n" _entries "${_entries}")
    file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/sw_units.json
        CONTENT "[\n${_entries}]\n")
    set(SW_BIN ${SW_BIN} PARENT_SCOPE)
    set(RP6502_SOFT_CPU ON PARENT_SCOPE)
endfunction()
