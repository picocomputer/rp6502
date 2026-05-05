cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Project definitions for both RIA and VIA.
# Please change name for hardware forks.
set(RP6502_PROJECT_DEFINITIONS
    RP6502_NAME="Picocomputer 6502"
    RP6502_VERSION=""
    RP6502_CODE_PAGE=437
    RP6502_LOCALE=en
    RP6502_EXFAT=0
)

# Project subdirectories.
set(RP6502_PROJECT_DIRECTORIES
    src/ria
    src/vga
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

# Regenerate rp6502_version.h whenever any of `tgt`'s objects change (except
# `src`'s own .o, which would self-cycle). OUTPUT is a stamp file, hdr is a
# BYPRODUCT — the BYPRODUCT designation keeps hdr out of cmake's
# cmake_object_order_depends node so OBJECT_DEPENDS on `src` doesn't cycle
# back through main.c.o etc. Script writes hdr only when content changes,
# so a no-op rebuild stays a no-op.
function(rp6502_use_version_header tgt src)
    set(hdr ${CMAKE_CURRENT_BINARY_DIR}/rp6502_version.h)
    set(stamp ${CMAKE_CURRENT_BINARY_DIR}/rp6502_version.stamp)
    target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

    string(REPLACE "." "\\." _esc "${src}")
    add_custom_command(
        OUTPUT ${stamp}
        BYPRODUCTS ${hdr}
        DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/${src}
            $<FILTER:$<TARGET_OBJECTS:${tgt}>,EXCLUDE,${_esc}\\.o$>
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

include(pico_sdk_import.cmake)

set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/src/tinyusb)

project(RP6502 C CXX ASM)

pico_sdk_init()

foreach(_dir IN LISTS RP6502_PROJECT_DIRECTORIES)
    add_subdirectory(${_dir})
endforeach()
