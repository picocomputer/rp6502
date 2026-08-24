# One version string, three forms, shared by every tree that ships something.
#
#     Version 0.31                a tagged build, -DRP6502_VERSION=0.31
#     CI 31666918326              an untagged CI build, -DRP6502_CI=<run id>
#     Aug 12 2026 20:17:46 PDT    a developer's own build
#
# The firmware root and src/core/emu both include this; src/host/pocket writes the
# same three forms into core.json, without the "Version " prefix the Pocket UI
# supplies itself.

include_guard(GLOBAL)

# -DRP6502_VERSION=<v> (release builds) overrides an empty default
set(RP6502_VERSION_VALUE "")
if(DEFINED RP6502_VERSION AND NOT RP6502_VERSION STREQUAL "")
    set(RP6502_VERSION_VALUE "${RP6502_VERSION}")
endif()

# -DRP6502_CI=<run id> stamps untagged CI builds; never cached so a
# reconfigure cannot resurrect another run's id.
set(RP6502_CI_VALUE "")
if(DEFINED RP6502_CI AND NOT RP6502_CI STREQUAL "")
    set(RP6502_CI_VALUE "${RP6502_CI}")
endif()
unset(RP6502_CI CACHE)

set(RP6502_GEN_VERSION_SCRIPT ${CMAKE_BINARY_DIR}/gen_rp6502_version_header.cmake)
file(WRITE ${RP6502_GEN_VERSION_SCRIPT} [[
if(VERSION)
    set(_stamp "Version ${VERSION}")
elseif(CI)
    set(_stamp "CI ${CI}")
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

# Configure-time forms, for the one consumer that cannot read a generated
# header: the Windows resource compiler. Same three forms, except a dev build
# is stamped when it was configured rather than when it was built, which is as
# close as a .rc can get.
if(RP6502_VERSION_VALUE)
    set(RP6502_VERSION_STAMP "Version ${RP6502_VERSION_VALUE}")
elseif(RP6502_CI_VALUE)
    set(RP6502_VERSION_STAMP "CI ${RP6502_CI_VALUE}")
else()
    string(TIMESTAMP RP6502_VERSION_STAMP "%b %d %Y %H:%M:%S %Z")
endif()

# FILEVERSION is four integers, so a run id and a timestamp have nowhere to go
# and only a tag can fill it. The strings beside it carry the stamp instead.
set(RP6502_VERSION_QUAD "0,0,0,0")
if(RP6502_VERSION_VALUE MATCHES "^([0-9]+)\\.([0-9]+)(\\.([0-9]+))?$")
    set(_patch "${CMAKE_MATCH_4}")
    if(NOT _patch)
        set(_patch 0)
    endif()
    set(RP6502_VERSION_QUAD "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${_patch},0")
    unset(_patch)
endif()

# Regenerate rp6502_version.h whenever any of `tgt`'s objects change.
# hdr is a BYPRODUCT to keep it out of cmake_object_order_depends so
# OBJECT_DEPENDS on `src` doesn't cycle back through the other objects.
# `src` may be relative to the current source dir or absolute.
function(rp6502_use_version_header tgt src)
    set(hdr ${CMAKE_CURRENT_BINARY_DIR}/rp6502_version.h)
    set(stamp ${CMAKE_CURRENT_BINARY_DIR}/rp6502_version.stamp)
    target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

    set(_src "${src}")
    if(NOT IS_ABSOLUTE "${_src}")
        set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
    endif()
    get_filename_component(_src_name "${_src}" NAME)

    # Generators other than these two get one stamp per configure. The tagged
    # and CI forms are fixed anyway; only a developer's timestamp goes stale,
    # and that beats refusing to build at all in an IDE that configures fine.
    if(NOT CMAKE_GENERATOR MATCHES "Make|Ninja")
        execute_process(COMMAND ${CMAKE_COMMAND}
            -DOUTPUT=${hdr}
            "-DVERSION=${RP6502_VERSION_VALUE}"
            "-DCI=${RP6502_CI_VALUE}"
            -P ${RP6502_GEN_VERSION_SCRIPT})
        return()
    endif()

    set(_deps ${_src})
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
            if(NOT "${_s}" STREQUAL "${_src}")
                list(APPEND _deps "${_s}")
                list(APPEND _impl C "${_s}")
            endif()
        endforeach()
        if(_impl)
            set(_impl IMPLICIT_DEPENDS ${_impl})
        endif()
    else() # Ninja
        string(REPLACE "." "\\." _esc "${_src_name}${CMAKE_C_OUTPUT_EXTENSION}")
        list(APPEND _deps $<FILTER:$<TARGET_OBJECTS:${tgt}>,EXCLUDE,${_esc}$>)
    endif()

    add_custom_command(
        OUTPUT ${stamp}
        BYPRODUCTS ${hdr}
        DEPENDS ${_deps}
        ${_impl}
        COMMAND ${CMAKE_COMMAND}
            -DOUTPUT=${hdr}
            "-DVERSION=${RP6502_VERSION_VALUE}"
            "-DCI=${RP6502_CI_VALUE}"
            -P ${RP6502_GEN_VERSION_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
        VERBATIM
    )

    set_property(SOURCE ${_src} APPEND PROPERTY OBJECT_DEPENDS ${hdr})
    add_custom_target(${tgt}_version_header ALL DEPENDS ${stamp})
endfunction()
