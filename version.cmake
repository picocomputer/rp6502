include_guard(GLOBAL)

# The option is off by default so that a developer's build of a tagged commit
# is not stamped as a release.
option(RP6502_VERSION_FROM_GIT "Take the version from a tag on HEAD" OFF)

set(RP6502_VERSION_VALUE "")
if(DEFINED RP6502_VERSION AND NOT RP6502_VERSION STREQUAL "")
    set(RP6502_VERSION_VALUE "${RP6502_VERSION}")
elseif(RP6502_VERSION_FROM_GIT)
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_CURRENT_LIST_DIR}
                describe --tags --exact-match HEAD
            OUTPUT_VARIABLE _tag OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _rc ERROR_QUIET)
        if(_rc EQUAL 0)
            string(REGEX REPLACE "^v" "" RP6502_VERSION_VALUE "${_tag}")
        endif()
        unset(_tag)
        unset(_rc)
    endif()
endif()

# RP6502_CI is removed from the cache so that a later configure without
# -DRP6502_CI does not reuse another run's id.
set(RP6502_CI_VALUE "")
if(DEFINED RP6502_CI AND NOT RP6502_CI STREQUAL "")
    set(RP6502_CI_VALUE "${RP6502_CI}")
endif()
unset(RP6502_CI CACHE)

# RP6502_GIT is removed from the cache so that a later configure without
# -DRP6502_GIT does not reuse an old commit's hash.
set(RP6502_GIT_VALUE "")
if(DEFINED RP6502_GIT AND NOT RP6502_GIT STREQUAL "")
    set(RP6502_GIT_VALUE "${RP6502_GIT}")
endif()
unset(RP6502_GIT CACHE)

set(RP6502_STAMP_SCRIPT ${CMAKE_BINARY_DIR}/rp6502_version_stamp.cmake)
file(WRITE ${RP6502_STAMP_SCRIPT} [[
if(STAMP_VERSION)
    set(_stamp_bare "${STAMP_VERSION}")
    set(_stamp "Version ${STAMP_VERSION}")
elseif(STAMP_CI)
    set(_stamp_bare "CI ${STAMP_CI}")
    set(_stamp "${_stamp_bare}")
elseif(STAMP_GIT)
    set(_stamp_bare "GIT ${STAMP_GIT}")
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

# These configure-time copies are for the Windows version.rc and the macOS
# Info.plist, which are configured rather than compiled, so a developer's build
# is stamped there with the time of the configure rather than of the build.
set(STAMP_VERSION "${RP6502_VERSION_VALUE}")
set(STAMP_CI "${RP6502_CI_VALUE}")
set(STAMP_GIT "${RP6502_GIT_VALUE}")
include(${RP6502_STAMP_SCRIPT})
set(RP6502_VERSION_STAMP "${_stamp}")
set(RP6502_VERSION_BARE "${_stamp_bare}")
unset(STAMP_VERSION)
unset(STAMP_CI)
unset(STAMP_GIT)
unset(_stamp)
unset(_stamp_bare)

# Windows FILEVERSION takes four integers and macOS CFBundleVersion takes
# dotted integers. A CI run id, a commit hash and a timestamp fit neither, so
# only a numeric tagged version fills these forms, and the free-form version
# strings beside them hold the full stamp.
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

# ARGN names further targets that are part of the program, such as a static
# library it links. Without them the header is regenerated only for changes to
# tgt, so a developer's build could report a time from before the code it is
# running.
#
# The header is a BYPRODUCT because CMake's Ninja generator adds an OUTPUT
# named in OBJECT_DEPENDS to the target's cmake_object_order_depends, which
# every object of the target waits on. The command depends on those objects,
# so an OUTPUT header would be a dependency cycle.
function(rp6502_use_version_header tgt src)
    set(hdr ${CMAKE_CURRENT_BINARY_DIR}/rp6502_version.h)
    set(stamp ${CMAKE_CURRENT_BINARY_DIR}/rp6502_version.stamp)
    target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

    set(_src "${src}")
    if(NOT IS_ABSOLUTE "${_src}")
        set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
    endif()
    get_filename_component(_src_name "${_src}" NAME)

    # With any other generator the header is written once per configure. That
    # leaves only a developer's timestamp stale, because the tagged, CI and GIT
    # forms do not change between builds.
    if(NOT CMAKE_GENERATOR MATCHES "Make|Ninja")
        execute_process(COMMAND ${CMAKE_COMMAND}
            -DOUTPUT=${hdr}
            -DSTAMP_SCRIPT=${RP6502_STAMP_SCRIPT}
            "-DSTAMP_VERSION=${RP6502_VERSION_VALUE}"
            "-DSTAMP_CI=${RP6502_CI_VALUE}"
            "-DSTAMP_GIT=${RP6502_GIT_VALUE}"
            -P ${RP6502_GEN_VERSION_SCRIPT})
        return()
    endif()

    set(_deps ${_src} ${RP6502_GEN_VERSION_SCRIPT} ${RP6502_STAMP_SCRIPT})
    set(_impl)
    set(_all_tgts ${tgt} ${ARGN})
    if(CMAKE_GENERATOR MATCHES "Make")
        # The custom target below has no include directories of its own, so
        # its IMPLICIT_DEPENDS scanner uses the directory's. tgt's include
        # directories are added to the directory for that scanner to find the
        # headers the sources include.
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
    else()
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
            "-DSTAMP_GIT=${RP6502_GIT_VALUE}"
            -P ${RP6502_GEN_VERSION_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
        VERBATIM
    )

    # CMake's Ninja generator lists a byproduct as an output of its command, so
    # the object is ordered after the header. The Makefile generator writes no
    # rule for a byproduct, so an object that depends on the header fails with
    # "No rule to make target" when the target is built by name before the
    # custom target below has run. The stamp is a real output under both
    # generators, so under Make the object depends on the stamp instead.
    if(CMAKE_GENERATOR MATCHES "Make")
        set_property(SOURCE ${_src} APPEND PROPERTY OBJECT_DEPENDS ${stamp})
    else()
        set_property(SOURCE ${_src} APPEND PROPERTY OBJECT_DEPENDS ${hdr})
    endif()
    add_custom_target(${tgt}_version_header ALL DEPENDS ${stamp})
endfunction()
