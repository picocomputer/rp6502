include(${RP6502_SRC}/core/gen.cmake)
rp6502_gen_c()
rp6502_gen_assets()
include(${RP6502_SRC}/core/log.cmake)
if(NOT RP6502_SW_LOG)
    set(RP6502_SW_LOG ${CMAKE_CURRENT_LIST_DIR}/sw/log.c)
endif()

find_program(RISCV_GCC riscv64-unknown-elf-gcc)
find_program(RISCV_OBJCOPY riscv64-unknown-elf-objcopy)
if(RISCV_GCC AND RISCV_OBJCOPY)
    set(RP6502_SOFT_CPU ON)
    set(SW_SRC ${CMAKE_CURRENT_LIST_DIR}/sw)
    set(SW_BIN ${RP6502_ASSETS}/sw.bin)
    file(GLOB_RECURSE SW_HEADERS CONFIGURE_DEPENDS
        ${RP6502_SRC}/core/*.h
        ${RP6502_SRC}/core/def/*.def
        ${CMAKE_CURRENT_LIST_DIR}/*.h)
    file(GLOB SW_SEAMS CONFIGURE_DEPENDS
        ${RP6502_SRC}/osal/*.h
        ${RP6502_SRC}/host/*.h)
    list(APPEND SW_HEADERS ${SW_SEAMS})
    set(SW_SOURCES
        ${SW_SRC}/crt0.S ${SW_SRC}/ver.c ${SW_SRC}/main.c
        ${SW_SRC}/apf.c ${SW_SRC}/aud.c
        ${SW_SRC}/sleep.c ${SW_SRC}/fab.c
        ${SW_SRC}/cfg.c
        ${RP6502_SRC}/core/com/com.c ${RP6502_SRC}/core/com/manifold.c ${RP6502_SRC}/core/com/tio.c ${RP6502_SRC}/core/com/rln.c ${SW_SRC}/ria.c ${SW_SRC}/phi2.c ${SW_SRC}/resb.c ${SW_SRC}/hid.c
        ${SW_SRC}/mem.c ${RP6502_SRC}/core/sys/timer.c
        ${RP6502_SRC}/core/sys/crc32.c
        ${RP6502_SRC}/core/sys/sst.c
        ${SW_SRC}/dir.c ${SW_SRC}/fs.c
        ${SW_SRC}/proc.c ${SW_SRC}/rom.c ${SW_SRC}/time.c
        ${SW_SRC}/trap.c ${RP6502_SW_LOG} ${SW_SRC}/vga.c ${SW_SRC}/vid.c
        ${RP6502_SRC}/core/aud/pcm.c
        ${RP6502_SRC}/core/aud/bel.c
        ${RP6502_SRC}/core/sys/pix.c
        ${RP6502_SRC}/core/rom/alias.c
        ${RP6502_SRC}/core/rom/asset.c
        ${RP6502_SRC}/core/rom/pump.c
        ${RP6502_SRC}/core/api/xreg0.c
        ${RP6502_SRC}/core/api/xreg1.c
        ${RP6502_SRC}/core/sys/random.c
        ${RP6502_SRC}/core/sys/ring.c
        ${RP6502_SRC}/core/sys/sys.c
        ${RP6502_SRC}/core/api/api.c
        ${RP6502_SRC}/core/api/arg.c
        ${RP6502_SRC}/core/api/attr.c
        ${RP6502_SRC}/core/api/proc.c
        ${RP6502_SRC}/core/api/save.c
        ${RP6502_SRC}/core/api/clk.c
        ${RP6502_SRC}/core/api/std.c
        ${RP6502_SRC}/core/api/dir.c
        ${RP6502_SRC}/core/api/ops.c
        ${RP6502_SRC}/core/str/path.c
        ${RP6502_SRC}/core/str/unicode.c
        ${RP6502_GEN}/unicode_oem.c
        ${RP6502_SRC}/core/hid/hid.c
        ${RP6502_SRC}/core/hid/keyboard.c
        ${RP6502_SRC}/core/hid/layout.c
        ${RP6502_GEN}/layout_db.c
        ${RP6502_SRC}/core/hid/keymap.c
        ${RP6502_SRC}/core/hid/vtkey.c
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
        ${RP6502_SRC}/core/term/color.c
        ${RP6502_SRC}/core/term/font.c
        ${RP6502_SRC}/core/term/term.c
        ${RP6502_SRC}/core/term/grid.c
        ${RP6502_SRC}/core/term/cells.c
        ${RP6502_SRC}/core/sys/version.c)
    rp6502_log_flags(SW_LOG_FLAGS)
    # AUD_NATIVE_RATE is the 48 kHz the fabric mixes at, which pcm_step
    # divides by. RP6502_SST builds the savestate's columns of the stdio
    # rows, as on the emulators.
    # The firmware's version, for XTVERSION, is stamped as it is built.
    add_custom_command(OUTPUT ${SW_BIN}
        COMMAND ${CMAKE_COMMAND}
            -DOUTPUT=${RP6502_ASSETS}/rp6502_version.h
            -DSTAMP_SCRIPT=${RP6502_STAMP_SCRIPT}
            "-DSTAMP_VERSION=${RP6502_VERSION_VALUE}"
            "-DSTAMP_CI=${RP6502_CI_VALUE}"
            "-DSTAMP_GIT=${RP6502_GIT_VALUE}"
            -P ${RP6502_GEN_VERSION_SCRIPT}
        COMMAND ${RISCV_GCC} -march=rv32imac_zicsr_zifencei -mabi=ilp32
            -msave-restore
            -Os -ffreestanding -nostartfiles
            --specs=picolibc.specs -DPICOLIBC_INTEGER_PRINTF_SCANF
            -ffunction-sections -fdata-sections -Wl,--gc-sections -flto=auto
            -Werror=implicit-function-declaration
            -I ${CMAKE_CURRENT_LIST_DIR}
            -I ${RP6502_SRC}
            -I ${RP6502_GEN}
            -I ${RP6502_ASSETS}
            "-DPICO_PROGRAM_NAME=\"RP6502-FPGA\""
            -DRP6502_EXFAT=0
            -DRP6502_LOCALE=EN
            -DRP6502_VGA_FABRIC
            -DAUD_NATIVE_RATE=48000
            -DRP6502_SST
            ${SW_LOG_FLAGS}
            -T ${SW_SRC}/link.ld
            -o ${RP6502_ASSETS}/sw.elf
            ${SW_SOURCES}
        COMMAND ${RISCV_OBJCOPY} -O binary ${RP6502_ASSETS}/sw.elf ${SW_BIN}
        DEPENDS ${SW_SOURCES} ${SW_HEADERS} ${SW_SRC}/link.ld
            ${RP6502_GEN}/rsmp_half.h
            ${RP6502_GEN}/pocket_version.h
        COMMENT "Cross-compiling the soft CPU firmware"
        VERBATIM)
    add_custom_target(sw_bin DEPENDS ${SW_BIN})
else()
    message(WARNING
        "riscv64-unknown-elf-gcc not found — soft CPU tests skipped.\n"
        "  sudo apt-get install gcc-riscv64-unknown-elf picolibc-riscv64-unknown-elf")
endif()
