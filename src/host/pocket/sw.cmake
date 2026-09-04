# The firmware the machine's soft CPU runs: this board's, cross-compiled for
# the Hazard3 in the fabric beside it. Every tree that boots the machine needs
# it, so it is here rather than inside the bitstream recipe.
#
# Toolchain per README: apt install gcc-riscv64-unknown-elf.
#
# RP6502_SOFT_CPU  the RISC-V toolchain is present, so anything that needs a
#                  booted soft CPU can be registered.

include(${RP6502_SRC}/core/assets.cmake)

find_program(RISCV_GCC riscv64-unknown-elf-gcc)
find_program(RISCV_OBJCOPY riscv64-unknown-elf-objcopy)
if(RISCV_GCC AND RISCV_OBJCOPY)
    set(RP6502_SOFT_CPU ON)
    set(SW_SRC ${CMAKE_CURRENT_LIST_DIR}/sw)
    set(SW_BIN ${RP6502_ASSETS}/sw.bin)
    # The firmware's own headers carry the hardware's addresses, so a window
    # that moves has to rebuild the image that writes to it; the rest are here
    # because this image compiles core sources. Broader than the include list
    # needs, which costs one gcc run and nothing re-verilates behind it.
    # CONFIGURE_DEPENDS because a plain GLOB is evaluated once: a new header
    # would go untracked, and a deleted one leaves ninja demanding a file no
    # rule can produce.
    # The .def files are headers by another name: this image compiles
    # core/str/str.c, which includes core/def/str_sys.def, so editing one has
    # to relink sw.bin. Only this build needs saying -- the RIA and the
    # emulator get it from gcc depfiles.
    file(GLOB_RECURSE SW_HEADERS CONFIGURE_DEPENDS
        ${RP6502_SRC}/core/*.h
        ${RP6502_SRC}/core/def/*.def
        ${CMAKE_CURRENT_LIST_DIR}/*.h)
    # The seams this image is built against: what an OS answers and what a
    # host owes. Not recursive, because the answers below them belong to other
    # machines and this one compiles its own.
    file(GLOB SW_SEAMS CONFIGURE_DEPENDS
        ${RP6502_SRC}/osal/*.h
        ${RP6502_SRC}/host/*.h)
    list(APPEND SW_HEADERS ${SW_SEAMS})
    set(SW_SOURCES
        ${SW_SRC}/crt0.S ${SW_SRC}/main.c
        ${SW_SRC}/apf.c ${SW_SRC}/aud.c
        ${SW_SRC}/sst.c
        ${SW_SRC}/cfg.c
        ${RP6502_SRC}/core/com/com.c ${SW_SRC}/com.c ${SW_SRC}/phi2.c ${SW_SRC}/resb.c ${SW_SRC}/font.c ${SW_SRC}/hid.c
        ${SW_SRC}/mem.c ${RP6502_SRC}/core/sys/timer.c
        ${RP6502_SRC}/core/sys/crc32.c
        ${SW_SRC}/dir.c ${SW_SRC}/fs.c
        ${SW_SRC}/proc.c ${SW_SRC}/rom.c ${SW_SRC}/time.c
        ${SW_SRC}/trap.c ${SW_SRC}/tty.c ${SW_SRC}/unicode.c ${SW_SRC}/vga.c ${SW_SRC}/vid.c
        ${RP6502_SRC}/core/aud/bel_presets.c
        ${SW_SRC}/bel.c
        ${RP6502_SRC}/core/sys/pix.c
        ${RP6502_SRC}/core/rom/asset.c
        ${RP6502_SRC}/core/rom/pump.c
        ${RP6502_SRC}/core/api/xreg0.c
        ${RP6502_SRC}/core/api/xreg1.c
        ${RP6502_SRC}/core/sys/random.c
        ${RP6502_SRC}/core/sys/sys.c
        ${RP6502_SRC}/core/api/api.c
        ${RP6502_SRC}/core/api/arg.c
        ${RP6502_SRC}/core/api/attr.c
        ${RP6502_SRC}/core/api/proc.c
        ${RP6502_SRC}/core/api/clk.c
        ${RP6502_SRC}/core/api/std.c
        ${RP6502_SRC}/core/api/dir.c
        ${RP6502_SRC}/core/api/ops.c
        ${RP6502_SRC}/core/str/unicode.c
        # The real HID drivers, told by apf.c what the dock holds. No
        # descriptor ever reaches this machine, so core/hid/parse.c is
        # not here. The layouts are an asset, so layout.c reads them rather
        # than linking twenty kilobytes of table into a 96 KB memory.
        ${RP6502_SRC}/core/hid/hid.c
        ${RP6502_SRC}/core/hid/keyboard.c
        ${RP6502_SRC}/core/hid/layout.c
        ${RP6502_SRC}/core/hid/keymap.c
        ${RP6502_SRC}/core/hid/mouse.c
        ${RP6502_SRC}/core/hid/gamepad.c
        ${RP6502_SRC}/core/hid/tablet.c
        ${RP6502_SRC}/core/str/rln.c
        ${RP6502_SRC}/core/str/str.c
        ${RP6502_SRC}/core/sys/config.c
        ${RP6502_SRC}/core/vga/canvas.c
        ${RP6502_SRC}/core/vga/mode/mode.c
        ${RP6502_SRC}/core/vga/mode/mode1.c
        ${RP6502_SRC}/core/vga/mode/mode2.c
        ${RP6502_SRC}/core/vga/mode/mode3.c
        ${RP6502_SRC}/core/vga/mode/mode4.c
        ${RP6502_SRC}/core/vga/mode/mode5.c
        ${RP6502_SRC}/core/term/color.c
        ${RP6502_SRC}/core/term/term.c)
    add_custom_command(OUTPUT ${SW_BIN}
        COMMAND ${RISCV_GCC} -march=rv32imac_zicsr_zifencei -mabi=ilp32
            # Prologues and epilogues become calls into libgcc's
            # __riscv_save_N/__riscv_restore_N instead of a run of
            # stores. Kilobytes of text for a few cycles a call, and the
            # whole firmware shares one TCM with the stack and the heap,
            # so text is the scarce thing here, not cycles.
            -msave-restore
            -Os -ffreestanding -nostartfiles
            # Integer-only printf AND scanf. Both halves are load-bearing:
            # the specs turns this one option into a --defsym for each, and
            # scanf is reachable -- strftime calls tzset, which parses the
            # TZ string with sscanf. Without the scanf defsym that resolves
            # to the double implementation and drags in ryu float parsing.
            --specs=picolibc.specs -DPICOLIBC_INTEGER_PRINTF_SCANF
            -ffunction-sections -fdata-sections -Wl,--gc-sections -flto
            # -flto turns a missing prototype into a miscompile of
            # unrelated code, and this line carried no -W at all.
            -Werror=implicit-function-declaration
            -I ${CMAKE_CURRENT_LIST_DIR}
            -I ${RP6502_SRC}
            -I ${RP6502_ASSETS}
            "-DPICO_PROGRAM_NAME=\"RP6502-FPGA\""
            # vendored ffconf.h tests it with #if; the other two roots
            # pass it and this one was relying on undefined-is-zero.
            -DRP6502_EXFAT=0
            # str.h's row default and str.c's fallback both stringize this;
            # undefined, the default becomes the macro's own name, too long
            # for the field and left unterminated.
            -DRP6502_LOCALE=EN
            -T ${SW_SRC}/link.ld -Wl,--no-warn-rwx-segments
            -o ${RP6502_ASSETS}/sw.elf
            ${SW_SOURCES}
        COMMAND ${RISCV_OBJCOPY} -O binary ${RP6502_ASSETS}/sw.elf ${SW_BIN}
        DEPENDS ${SW_SOURCES} ${SW_HEADERS} ${VID_FONT_ASSET_H}
            ${SW_SRC}/link.ld
        COMMENT "Cross-compiling the soft CPU firmware"
        VERBATIM)
    add_custom_target(sw_bin DEPENDS ${SW_BIN})
else()
    message(WARNING
        "riscv64-unknown-elf-gcc not found — soft CPU tests skipped.\n"
        "  sudo apt-get install gcc-riscv64-unknown-elf picolibc-riscv64-unknown-elf")
endif()
