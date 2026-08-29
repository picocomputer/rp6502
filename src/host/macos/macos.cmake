# The macOS host seam's packaging half.
#
# An .app is a directory holding the executable and an Info.plist, which CI
# used to assemble by hand around a binary the build had already produced.
# MACOSX_BUNDLE is CMake's support for it, so the build makes the thing that
# ships and the plist is a file in the tree rather than a shell heredoc.
#
# The rest of this host is host.c, window.c and gamepad.c, taken through
# host/posix like every other Unix. Only this part is Apple's.
#
# Included after emu.cmake.

# The plist is configured here rather than handed to MACOSX_BUNDLE_INFO_PLIST
# as a template: that substitutes only CMake's own MACOSX_BUNDLE_* names, and
# the version fields are ours.
function(rp6502_macos_bundle tgt)
    configure_file(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Info.plist.in Info.plist @ONLY)
    set_target_properties(${tgt} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
endfunction()
