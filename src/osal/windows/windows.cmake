# The Win32 seam, for the machines whose OS is one.
#
# rp6502_osal_windows(<target>)
#
# The desktop emulator and the libretro core share every file here. Unlike the
# POSIX seam there is no transport to choose: overlapped I/O is the kernel's
# own, with no helper threads to outlive an unloaded library, and the
# overlapped flag belongs to fs_std_open — so the transport could not leave
# fs.c. See its header. Nothing collides with ff.h on Win32 either, so the
# drive is one file rather than two.
#
# What is not here is what differs between machines rather than between
# operating systems: the console attach and the argv encoding, which the
# desktop answers in os_emu.c beside these and the libretro core does not
# answer at all.

include_guard(GLOBAL)

set(RP6502_OSAL_WINDOWS ${CMAKE_CURRENT_LIST_DIR})

function(rp6502_osal_windows target)
    target_sources(${target} PRIVATE
        ${RP6502_OSAL_WINDOWS}/dir.c
        ${RP6502_OSAL_WINDOWS}/errmap.c
        ${RP6502_OSAL_WINDOWS}/fs.c
        ${RP6502_OSAL_WINDOWS}/os.c)
    # CancelIoEx and SetFileInformationByHandle are Vista. PRIVATE, so a floor
    # set for these files cannot hide newer APIs from the window and pad code
    # that compiles into the emulator beside them.
    target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0601)
    if(NOT MSVC)
        return()
    endif()
    # What MSVC alone needs to compile the core beside this seam: shims
    # including a <strings.h> it has no system header for, in their own
    # directory because it goes on every consumer's include path and the host's
    # own headers are not ours to publish. MinGW takes none of it.
    target_include_directories(${target} PUBLIC ${RP6502_OSAL_WINDOWS}/msvc)
    target_compile_options(${target} PUBLIC /utf-8 /experimental:c11atomics /FIcompat.h)
    # Shared firmware idioms MSVC dislikes but GCC/Clang accept: #pragma GCC (C4068) and
    # `return void_expr;` from a void function (C4098). GCC gates any real value-return.
    target_compile_options(${target} PRIVATE /wd4068 /wd4098)
    target_compile_definitions(${target} PUBLIC _CRT_SECURE_NO_WARNINGS)
endfunction()
