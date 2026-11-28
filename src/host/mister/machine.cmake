include_guard(GLOBAL)

set(RP6502_MISTER_CORE ${CMAKE_CURRENT_LIST_DIR}/core)
set(RP6502_MISTER_TREE ${CMAKE_CURRENT_LIST_DIR}/tree)

set(RP6502_MISTER_SYS_REPO "" CACHE PATH
    "A Template_MiSTer clone holding the commit in src/host/mister/sys.pin")

# rp6502_mister_rtl(<out>)
#
# Sets <out> to the RTL the MiSTer tree copies into its flat rtl/ folder,
# in the order files.qip lists it: the machine, the soft CPU's hazard3
# modules and the MiSTer glue. Call it after rtl.cmake. In one folder two
# files of one name would be one file, so that is an error, and it is the
# error a hazard3 override would make if hazard3's own copy were listed
# as well.
function(rp6502_mister_rtl out)
    set(rtl ${RP6502_RTL_SOURCES} ${RP6502_RTL_HAZARD3}
        ${RP6502_MISTER_CORE}/mister_video.sv
        # The test card leaves the tree when the machine takes its place.
        ${RP6502_ROOT}/tests/host/mister/mister_bars.sv)
    file(GLOB_RECURSE tree_rtl ${RP6502_MISTER_TREE}/rtl/*)
    set(seen)
    foreach(src ${rtl} ${RP6502_RTL_INCLUDES} ${tree_rtl})
        get_filename_component(name ${src} NAME)
        if(name IN_LIST seen)
            message(FATAL_ERROR "the MiSTer tree's rtl/ would get two files "
                "named ${name}; ${src} is the second")
        endif()
        list(APPEND seen ${name})
    endforeach()
    set(${out} ${rtl} PARENT_SCOPE)
endfunction()

# rp6502_mister_sys(<out> WANTS <what>)
#
# Sets <out> to a folder holding sys/ of the Template_MiSTer commit in
# sys.pin, or to nothing, with a warning that <what> is left out, when no
# repository has it. The folder is named for the tree's git id, and sys/
# comes from the branch mister of this repository, which publishes the
# core, or else from RP6502_MISTER_SYS_REPO at the pinned commit. Either
# must hold exactly the pinned tree, so the sys/ a build uses is
# upstream's, unedited.
function(rp6502_mister_sys out)
    cmake_parse_arguments(PARSE_ARGV 1 S "" "WANTS" "")
    set(pin ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sys.pin)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${pin})
    file(STRINGS ${pin} lines REGEX "^(commit|tree) ")
    foreach(line ${lines})
        string(REGEX MATCH "^([a-z]+) ([0-9a-f]+)$" _ ${line})
        set(${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
    endforeach()
    set(dir ${CMAKE_BINARY_DIR}/sys-${tree})
    if(EXISTS ${dir}/sys_top.v)
        set(${out} ${dir} PARENT_SCOPE)
        return()
    endif()

    find_package(Git QUIET)
    set(from)
    if(Git_FOUND)
        foreach(spec "${RP6502_ROOT};mister:sys" "${RP6502_ROOT};origin/mister:sys"
                "${RP6502_MISTER_SYS_REPO};${commit}:sys")
            list(GET spec 0 repo)
            list(GET spec 1 rev)
            if(NOT repo)
                continue()
            endif()
            execute_process(COMMAND ${GIT_EXECUTABLE} -C ${repo}
                    rev-parse --verify --quiet ${rev}
                OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE rc ERROR_QUIET)
            if(NOT rc AND id STREQUAL tree)
                set(from ${repo})
                break()
            endif()
        endforeach()
    endif()
    if(NOT from)
        message(WARNING "sys/ ${tree} is absent - ${S_WANTS} needs it. "
            "The rp6502 branch mister does not have it and "
            "RP6502_MISTER_SYS_REPO ('${RP6502_MISTER_SYS_REPO}') has no "
            "commit ${commit}. Clone "
            "https://github.com/MiSTer-devel/Template_MiSTer and configure "
            "with -DRP6502_MISTER_SYS_REPO=<the clone>.")
        set(${out} "" PARENT_SCOPE)
        return()
    endif()

    message(STATUS "Taking sys/ ${tree} from ${from}")
    file(REMOVE_RECURSE ${dir}.part)
    file(MAKE_DIRECTORY ${dir}.part)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${from} archive
            --format=tar -o ${dir}.part/sys.tar ${tree}
        RESULT_VARIABLE rc)
    if(rc)
        message(FATAL_ERROR "git archive of sys/ ${tree} from ${from} failed")
    endif()
    file(ARCHIVE_EXTRACT INPUT ${dir}.part/sys.tar
        DESTINATION ${dir}.part/sys)
    file(RENAME ${dir}.part/sys ${dir})
    file(REMOVE_RECURSE ${dir}.part)
    set(${out} ${dir} PARENT_SCOPE)
endfunction()
