# rp6502_osal_windows(<target> TRANSPORT overlapped|sync)
#
# The Win32 half of osal, shared by the desktop emulator and the libretro core.
# The transport is the choice the POSIX build makes between fs_aio.c and
# fs_sync.c, made with a define rather than a file because FILE_FLAG_OVERLAPPED
# belongs to the handle and both arms live in fs.c. See its header for which
# host takes which.
#
# console.c is not listed, because only a host that owns a terminal links it
# and it takes the console control handler and an atexit from the process to do
# so. A host that wants it adds it itself.

include_guard(GLOBAL)

set(RP6502_OSAL_WINDOWS ${CMAKE_CURRENT_LIST_DIR})

function(rp6502_osal_windows target)
    cmake_parse_arguments(W "" "TRANSPORT" "" ${ARGN})
    if(NOT W_TRANSPORT MATCHES "^(overlapped|sync)$")
        message(FATAL_ERROR "rp6502_osal_windows(${target}): TRANSPORT is overlapped or sync")
    endif()
    if(W_TRANSPORT STREQUAL "sync")
        target_compile_definitions(${target} PRIVATE RP6502_FS_SYNC)
    endif()
    target_sources(${target} PRIVATE
        ${RP6502_OSAL_WINDOWS}/dir.c
        ${RP6502_OSAL_WINDOWS}/errmap.c
        ${RP6502_OSAL_WINDOWS}/fs.c
        ${RP6502_OSAL_WINDOWS}/os.c)
    # CancelIoEx and SetFileInformationByHandle in fs.c, and
    # CreateWaitableTimerExW in os.c, are declared only when _WIN32_WINNT names
    # Vista or later, so the floor cannot go below 0x0600. Nothing here needs
    # Windows 7; 0x0601 is a margin above the real floor. PRIVATE, so this
    # floor cannot hide newer APIs from the window and pad code compiled beside
    # these files.
    target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0601)
    if(NOT MSVC)
        return()
    endif()
    # What MSVC alone needs to compile the shared sources, including a
    # <strings.h> it has no system header for. Its own directory, because it
    # goes on every consumer's include path and the host's own headers are not
    # ours to publish. MinGW needs none of it.
    target_include_directories(${target} PUBLIC ${RP6502_OSAL_WINDOWS}/msvc)
    # /Zc:preprocessor because core/sys/debug_log.h pastes a macro that expands
    # to two arguments, which the traditional preprocessor keeps as one.
    target_compile_options(${target} PUBLIC /utf-8 /experimental:c11atomics /FIcompat.h /Zc:preprocessor)
    # Idioms in the shared sources that GCC and Clang accept: #pragma GCC is
    # C4068, and returning a void expression from a void function is C4098.
    target_compile_options(${target} PRIVATE /wd4068 /wd4098)
    target_compile_definitions(${target} PUBLIC _CRT_SECURE_NO_WARNINGS)
endfunction()
