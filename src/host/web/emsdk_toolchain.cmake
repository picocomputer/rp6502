# Emscripten's toolchain, with the fetch in front of it.
#
# The preset has to name a toolchain file, and CMake reads a preset before it
# reads anything of ours — so pointing it straight at vendor/emsdk meant the
# web build failed before a single line of this project ran. It names this
# instead. A toolchain file is an ordinary script, so it can go and get the
# thing it is about to include.
#
# Two stages, because the submodule is only the installer: the checkout
# carries emsdk itself and not one file of the toolchain, which arrives with
# install and activate. That is why the sentinel is Emscripten's own cmake
# file rather than the directory — an install that died halfway leaves plenty
# behind to fool a shallower test.
#
# Re-read on every try_compile, so the path where everything is already here
# has to be a single stat.

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
    # Minutes and a few hundred megabytes. Said out loud, because a silent
    # configure that takes this long reads as a hang.
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
