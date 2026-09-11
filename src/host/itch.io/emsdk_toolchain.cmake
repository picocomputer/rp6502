# Emscripten's toolchain, with the fetch in front of it.
#
# CMakeLists.txt names this file rather than Emscripten's own toolchain file,
# because that one does not exist until the install below has run. A toolchain
# file is an ordinary script, so it can fetch what it then includes.
#
# The submodule checkout is only the installer: it carries emsdk itself and
# not one file of the toolchain, which arrives with install and activate. The
# sentinel is therefore Emscripten's own cmake file rather than the directory,
# because an install that died halfway leaves plenty behind to satisfy a
# shallower test.
#
# CMake re-reads this on every try_compile, so the path where everything is
# already present must cost a single stat.

get_filename_component(RP6502_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
set(EMSDK ${RP6502_ROOT}/vendor/emsdk)
set(EMSDK_TOOLCHAIN
    ${EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake)

if(NOT EXISTS ${EMSDK_TOOLCHAIN})
    include(${RP6502_ROOT}/submodules.cmake)
    rp6502_submodule(vendor/emsdk SENTINEL emsdk.py WANTS "the web build")
    if(CMAKE_HOST_WIN32)
        set(EMSDK_EXE ${EMSDK}/emsdk.bat)
    else()
        set(EMSDK_EXE ${EMSDK}/emsdk)
    endif()
    # The install takes minutes and a few hundred megabytes, so it is announced
    # rather than left to look like a hung configure.
    message(STATUS "Installing the Emscripten toolchain (a few hundred MB)")
    execute_process(COMMAND ${EMSDK_EXE} install latest RESULT_VARIABLE _rc)
    if(NOT _rc)
        execute_process(COMMAND ${EMSDK_EXE} activate latest
            RESULT_VARIABLE _rc)
    endif()
    if(NOT EXISTS ${EMSDK_TOOLCHAIN})
        message(FATAL_ERROR
            "the Emscripten toolchain is absent — the web build needs it.\n"
            "  ${EMSDK_EXE} install latest\n"
            "  ${EMSDK_EXE} activate latest")
    endif()
endif()

include(${EMSDK_TOOLCHAIN})
