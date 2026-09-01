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
# operating systems: the frame-pacer sleep, the console attach, and the
# argv encoding. Each host answers those in its own host.c.
#
# Included after emu.cmake: it adds to emu_core.

target_sources(emu_core PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/dir.c
    ${CMAKE_CURRENT_LIST_DIR}/errmap.c
    ${CMAKE_CURRENT_LIST_DIR}/fs.c
    ${CMAKE_CURRENT_LIST_DIR}/host.c)

# CancelIoEx and SetFileInformationByHandle are Vista. PRIVATE, so a floor
# set for these files cannot hide newer APIs from the window and pad code
# that compiles into the emulator beside them.
target_compile_definitions(emu_core PRIVATE _WIN32_WINNT=0x0601)

# The version metadata Explorer shows in Properties > Details. The template is
# this host's, because a .rc is Windows source that windres and rc.exe compile
# and the linker binds into the image. Enabling the RC language is not: a
# language is enabled for a directory, so the machine does that and calls this.
function(rp6502_windows_version_resource tgt)
    configure_file(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/version.rc.in version.rc @ONLY)
    target_sources(${tgt} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
endfunction()
