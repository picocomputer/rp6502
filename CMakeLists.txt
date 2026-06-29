cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Project definitions for both RIA and VIA.
# Please change name for hardware forks.
# Creator flag hides MAC and SSID.
# RP6502_LOCALE is the build default locale.
set(RP6502_PROJECT_DEFINITIONS
    RP6502_NAME="Picocomputer 6502"
    RP6502_VERSION=""
    RP6502_LOCALE=EN
    RP6502_EXFAT=0
    RP6502_CREATOR=0
)

# The shared src/ and vendor/ trees live beside this root CMakeLists (the
# firmware build). Reference sources and vendored deps via RP6502_ROOT.
get_filename_component(RP6502_ROOT ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)

# Project subdirectories (under ${RP6502_ROOT}/src).
set(RP6502_PROJECT_DIRECTORIES
    ria
    vga
)

# Force wrappers defined by pico_printf and pico_stdlib to be
# preserved during link-time interprocedural optimizations.
set(IPO_PRINTF_LINK_OPTIONS
    -Wl,-u,__wrap_printf
    -Wl,-u,__wrap_vprintf
    -Wl,-u,__wrap_puts
    -Wl,-u,__wrap_putchar
    -Wl,-u,__wrap_getchar
    -Wl,-u,__wrap_sprintf
    -Wl,-u,__wrap_snprintf
    -Wl,-u,__wrap_vsnprintf
)

set(RP6502_VERSION_VALUE "")
foreach(_def IN LISTS RP6502_PROJECT_DEFINITIONS)
    if(_def MATCHES "^RP6502_VERSION=\"(.*)\"$")
        set(RP6502_VERSION_VALUE "${CMAKE_MATCH_1}")
    endif()
endforeach()

set(RP6502_GEN_VERSION_SCRIPT ${CMAKE_BINARY_DIR}/gen_rp6502_version_header.cmake)
file(WRITE ${RP6502_GEN_VERSION_SCRIPT} [[
if(VERSION)
    set(_stamp "Version ${VERSION}")
else()
    string(TIMESTAMP _stamp "%b %d %Y %H:%M:%S %Z")
endif()
set(_new "#undef RP6502_VERSION\n#define RP6502_VERSION \"${_stamp}\"\n")
set(_old "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" _old)
endif()
if(NOT _new STREQUAL _old)
    file(WRITE "${OUTPUT}" "${_new}")
endif()
]])

# Regenerate rp6502_version.h whenever any of `tgt`'s objects change.
# hdr is a BYPRODUCT to keep it out of cmake_object_order_depends so
# OBJECT_DEPENDS on `src` doesn't cycle back through the other objects.
function(rp6502_use_version_header tgt src)
    set(hdr ${CMAKE_CURRENT_BINARY_DIR}/rp6502_version.h)
    set(stamp ${CMAKE_CURRENT_BINARY_DIR}/rp6502_version.stamp)
    target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

    set(_deps ${CMAKE_CURRENT_SOURCE_DIR}/${src})
    set(_impl)
    if(CMAKE_GENERATOR MATCHES "Make")
        # IMPLICIT_DEPENDS' C scanner uses the directory's include dirs,
        # not the target's — mirror them so #include "ria.h" etc. resolve.
        get_target_property(_inc_dirs ${tgt} INCLUDE_DIRECTORIES)
        if(_inc_dirs)
            include_directories(${_inc_dirs})
        endif()
        get_target_property(_srcs ${tgt} SOURCES)
        foreach(_s IN LISTS _srcs)
            if(NOT IS_ABSOLUTE "${_s}")
                set(_s "${CMAKE_CURRENT_SOURCE_DIR}/${_s}")
            endif()
            if(NOT "${_s}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
                list(APPEND _deps "${_s}")
                list(APPEND _impl C "${_s}")
            endif()
        endforeach()
        if(_impl)
            set(_impl IMPLICIT_DEPENDS ${_impl})
        endif()
    elseif(CMAKE_GENERATOR MATCHES "Ninja")
        string(REPLACE "." "\\." _esc "${src}${CMAKE_C_OUTPUT_EXTENSION}")
        list(APPEND _deps $<FILTER:$<TARGET_OBJECTS:${tgt}>,EXCLUDE,${_esc}$>)
    else()
        message(FATAL_ERROR
            "rp6502_use_version_header: unsupported CMake generator "
            "'${CMAKE_GENERATOR}'. Use 'Ninja' or 'Unix Makefiles'.")
    endif()

    add_custom_command(
        OUTPUT ${stamp}
        BYPRODUCTS ${hdr}
        DEPENDS ${_deps}
        ${_impl}
        COMMAND ${CMAKE_COMMAND}
            -DOUTPUT=${hdr}
            "-DVERSION=${RP6502_VERSION_VALUE}"
            -P ${RP6502_GEN_VERSION_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
        VERBATIM
    )

    set_property(SOURCE ${src} APPEND PROPERTY OBJECT_DEPENDS ${hdr})
    add_custom_target(${tgt}_version_header ALL DEPENDS ${stamp})
endfunction()

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

set(PICO_TINYUSB_PATH ${RP6502_ROOT}/vendor/tinyusb)

project(RP6502 C CXX ASM)

pico_sdk_init()

foreach(_dir IN LISTS RP6502_PROJECT_DIRECTORIES)
    add_subdirectory(${RP6502_ROOT}/src/${_dir} ${_dir})
endforeach()
