# The Win32 host seam, for the hosts whose OS is one.
#
# The desktop emulator and the libretro core share every file here. Unlike
# host/posix there is no transport to choose: overlapped I/O is the kernel's
# own, with no helper threads to outlive an unloaded library, and the
# overlapped flag belongs to fs_std_open — so the transport could not leave
# fs.c. See its header. Nothing collides with ff.h on Win32 either, so the
# drive is one file rather than two.
#
# What is not here is what differs between hosts rather than between
# operating systems: the console attach and the argv encoding. Each host
# answers those in its own os.c.
#
# Included after emu.cmake: it adds to emu_core.

target_sources(emu_core PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/dir.c
    ${CMAKE_CURRENT_LIST_DIR}/errmap.c
    ${CMAKE_CURRENT_LIST_DIR}/fs.c
    ${CMAKE_CURRENT_LIST_DIR}/os.c)

# CancelIoEx and SetFileInformationByHandle are Vista. PRIVATE, so a floor
# set for these files cannot hide newer APIs from the window and pad code
# that compiles into the emulator beside them.
target_compile_definitions(emu_core PRIVATE _WIN32_WINNT=0x0601)

# What MSVC alone needs to compile the core beside this seam: shims including a
# <strings.h> it has no system header for, in their own directory because it
# goes on every consumer's include path and the host's own headers are not ours
# to publish. MinGW takes none of it.
if(MSVC)
    target_include_directories(emu_core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/msvc)
    target_compile_options(emu_core PUBLIC /utf-8 /experimental:c11atomics /FIcompat.h)
    # Shared firmware idioms MSVC dislikes but GCC/Clang accept: #pragma GCC (C4068) and
    # `return void_expr;` from a void function (C4098). GCC gates any real value-return.
    target_compile_options(emu_core PRIVATE /wd4068 /wd4098)
    target_compile_definitions(emu_core PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()
