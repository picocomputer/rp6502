# One version string, three forms, shared by every tree that ships something.
#
#     Version 0.31                a tagged build, -DRP6502_VERSION=0.31
#     CI 31666918326              an untagged CI build, -DRP6502_CI=<run id>
#     Aug 12 2026 20:17:46 PDT    a developer's own build
#
# The firmware root and every machine root include this; src/host/pocket writes
# the same three forms into core.json, without the "Version " prefix the Pocket
# UI supplies itself.

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

# The three forms, decided once. Everything that wants one includes this --
# the header generator below, the Pocket's core.json stamper, and this file
# itself for the configure-time copies -- so a fourth spelling of the ladder
# cannot drift away from the other three.
#
#   _stamp       what the machine says it is
#   _stamp_bare  the same, without the word a UI supplies itself. Only the
#                tagged form differs; "CI <id>" and a timestamp read the same
#                either way.
set(RP6502_STAMP_SCRIPT ${CMAKE_BINARY_DIR}/rp6502_version_stamp.cmake)
file(WRITE ${RP6502_STAMP_SCRIPT} [[
if(STAMP_VERSION)
    set(_stamp_bare "${STAMP_VERSION}")
    set(_stamp "Version ${STAMP_VERSION}")
elseif(STAMP_CI)
    set(_stamp_bare "CI ${STAMP_CI}")
    set(_stamp "${_stamp_bare}")
else()
    string(TIMESTAMP _stamp_bare "%b %d %Y %H:%M:%S %Z")
    set(_stamp "${_stamp_bare}")
endif()
]])

set(RP6502_GEN_VERSION_SCRIPT ${CMAKE_BINARY_DIR}/gen_rp6502_version_header.cmake)
file(WRITE ${RP6502_GEN_VERSION_SCRIPT} [[
include("${STAMP_SCRIPT}")
set(_new "#undef RP6502_VERSION\n#define RP6502_VERSION \"${_stamp}\"\n")
string(APPEND _new "#undef RP6502_VERSION_BARE\n#define RP6502_VERSION_BARE \"${_stamp_bare}\"\n")
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
set(STAMP_VERSION "${RP6502_VERSION_VALUE}")
set(STAMP_CI "${RP6502_CI_VALUE}")
include(${RP6502_STAMP_SCRIPT})
set(RP6502_VERSION_STAMP "${_stamp}")
set(RP6502_VERSION_BARE "${_stamp_bare}")
unset(STAMP_VERSION)
unset(STAMP_CI)
unset(_stamp)
unset(_stamp_bare)

# Two more forms for the fields an OS insists are numbers: a Windows
# FILEVERSION quad and a CFBundleVersion dotted string. A run id and a
# timestamp are neither, so only a tag can fill them and the free-form strings
# beside them carry the stamp instead.
set(RP6502_VERSION_DOTTED "0.0")
if(RP6502_VERSION_VALUE MATCHES "^[0-9]+\\.[0-9]+(\\.[0-9]+)?$")
    set(RP6502_VERSION_DOTTED "${RP6502_VERSION_VALUE}")
endif()

set(RP6502_VERSION_QUAD "0,0,0,0")
if(RP6502_VERSION_VALUE MATCHES "^([0-9]+)\\.([0-9]+)(\\.([0-9]+))?$")
    set(_patch "${CMAKE_MATCH_4}")
    if(NOT _patch)
        set(_patch 0)
    endif()
    set(RP6502_VERSION_QUAD "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${_patch},0")
    unset(_patch)
endif()

# Regenerate rp6502_version.h whenever any object of this program changes.
# hdr is a BYPRODUCT to keep it out of cmake_object_order_depends so
# OBJECT_DEPENDS on `src` doesn't cycle back through the other objects.
# `src` may be relative to the current source dir or absolute.
#
# ARGN names further targets whose objects are also this program -- a static
# library it links. Without them a developer's timestamp tracks only the
# objects of `tgt` itself, so a build could report a time from before the code
# it is running; the two desktop roots keep most of the machine in emu_core.
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
            -DSTAMP_SCRIPT=${RP6502_STAMP_SCRIPT}
            "-DSTAMP_VERSION=${RP6502_VERSION_VALUE}"
            "-DSTAMP_CI=${RP6502_CI_VALUE}"
            -P ${RP6502_GEN_VERSION_SCRIPT})
        return()
    endif()

    # The generators too: a change to how the stamp is decided has to reach
    # the header, and nothing else in _deps would say so.
    set(_deps ${_src} ${RP6502_GEN_VERSION_SCRIPT} ${RP6502_STAMP_SCRIPT})
    set(_impl)
    set(_all_tgts ${tgt} ${ARGN})
    if(CMAKE_GENERATOR MATCHES "Make")
        # IMPLICIT_DEPENDS' C scanner uses the directory's include dirs,
        # not the target's — mirror them so #include "ria.h" etc. resolve.
        get_target_property(_inc_dirs ${tgt} INCLUDE_DIRECTORIES)
        if(_inc_dirs)
            include_directories(${_inc_dirs})
        endif()
        foreach(_t IN LISTS _all_tgts)
            get_target_property(_srcs ${_t} SOURCES)
            get_target_property(_tdir ${_t} SOURCE_DIR)
            foreach(_s IN LISTS _srcs)
                if(NOT IS_ABSOLUTE "${_s}")
                    set(_s "${_tdir}/${_s}")
                endif()
                if(NOT "${_s}" STREQUAL "${_src}")
                    list(APPEND _deps "${_s}")
                    list(APPEND _impl C "${_s}")
                endif()
            endforeach()
        endforeach()
        if(_impl)
            set(_impl IMPLICIT_DEPENDS ${_impl})
        endif()
    else() # Ninja
        string(REPLACE "." "\\." _esc "${_src_name}${CMAKE_C_OUTPUT_EXTENSION}")
        foreach(_t IN LISTS _all_tgts)
            list(APPEND _deps $<FILTER:$<TARGET_OBJECTS:${_t}>,EXCLUDE,${_esc}$>)
        endforeach()
    endif()

    add_custom_command(
        OUTPUT ${stamp}
        BYPRODUCTS ${hdr}
        DEPENDS ${_deps}
        ${_impl}
        COMMAND ${CMAKE_COMMAND}
            -DOUTPUT=${hdr}
            -DSTAMP_SCRIPT=${RP6502_STAMP_SCRIPT}
            "-DSTAMP_VERSION=${RP6502_VERSION_VALUE}"
            "-DSTAMP_CI=${RP6502_CI_VALUE}"
            -P ${RP6502_GEN_VERSION_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
        VERBATIM
    )

    set_property(SOURCE ${_src} APPEND PROPERTY OBJECT_DEPENDS ${hdr})
    add_custom_target(${tgt}_version_header ALL DEPENDS ${stamp})
endfunction()
