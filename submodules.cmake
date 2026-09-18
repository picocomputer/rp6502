include_guard(GLOBAL)

option(RP6502_FETCH_SUBMODULES "Let CMake fetch missing submodules" ON)
option(RP6502_STRICT_SUBMODULES "Optional submodules become required" OFF)
set(RP6502_SUBMODULE_DEPTH "1" CACHE STRING "Submodule clone depth, 0 for all")

# An uninitialized submodule is an empty directory and EXISTS is TRUE for it,
# so a submodule with no SENTINEL is checked by globbing its directory for
# content.
macro(_rp6502_submodule_ready out)
    if(S_SENTINEL)
        if(EXISTS ${_dir}/${S_SENTINEL})
            set(${out} TRUE)
        else()
            set(${out} FALSE)
        endif()
    else()
        file(GLOB _content ${_dir}/*)
        if(_content)
            set(${out} TRUE)
        else()
            set(${out} FALSE)
        endif()
    endif()
endmacro()

function(rp6502_submodule path)
    cmake_parse_arguments(S "OPTIONAL" "SENTINEL;SPARSE;SUPER;TARGET;WANTS;RESULT"
        "" ${ARGN})
    if(NOT S_SUPER)
        set(S_SUPER ${RP6502_ROOT})
    endif()
    set(_dir ${S_SUPER}/${path})
    if(S_TARGET AND NOT TARGET ${S_TARGET})
        add_custom_target(${S_TARGET}
            COMMAND ${CMAKE_COMMAND}
                -DRP6502_ROOT=${RP6502_ROOT}
                -DFETCH_SUPER=${S_SUPER} -DFETCH_PATH=${path}
                -DFETCH_SPARSE=${S_SPARSE}
                -P ${RP6502_ROOT}/submodules.cmake
            COMMENT "Fetching ${path}"
            VERBATIM)
    endif()

    _rp6502_submodule_ready(_ok)
    if(_ok)
        set(${S_RESULT} TRUE PARENT_SCOPE)
        return()
    endif()

    set(_how "git -C ${S_SUPER} submodule update --init ${path}")
    if(S_TARGET)
        set(_how "cmake --build <dir> --target ${S_TARGET}")
    endif()
    set(_why "")

    if(NOT RP6502_FETCH_SUBMODULES)
        set(_why "RP6502_FETCH_SUBMODULES is OFF")
    elseif(NOT EXISTS ${S_SUPER}/.git)
        set(_why "${S_SUPER} is not a git checkout")
    else()
        find_package(Git QUIET)
        if(NOT Git_FOUND)
            set(_why "git was not found")
        else()
            rp6502_submodule_fetch(${S_SUPER} ${path} "${S_SPARSE}")
            if(S_SPARSE)
                _rp6502_submodule_ready(_ok)
                if(NOT _ok)
                    rp6502_submodule_fetch(${S_SUPER} ${path} "")
                endif()
            endif()
        endif()
    endif()

    # Readiness is checked again instead of relying on git's exit status,
    # because git submodule update exits with success when it skips a
    # submodule whose submodule.<name>.update is none.
    _rp6502_submodule_ready(_ok)
    if(NOT _ok)
        set(_msg "${path} is absent")
        if(S_WANTS)
            set(_msg "${_msg} — ${S_WANTS} needs it")
        endif()
        if(_why)
            set(_msg "${_msg} (${_why})")
        endif()
        set(_msg "${_msg}.\n  ${_how}")
        if(S_OPTIONAL AND NOT RP6502_STRICT_SUBMODULES)
            message(STATUS "${_msg}")
        else()
            message(FATAL_ERROR "${_msg}")
        endif()
    endif()
    set(${S_RESULT} ${_ok} PARENT_SCOPE)
endfunction()

# Two build trees can be configured at the same time with submodules missing,
# and both CMake processes would then run git in the same checkout. The lock
# file is created in the checkout's .git directory, so no lock is taken where
# .git is a file, as it is in a submodule such as vendor/cppdap or in a linked
# worktree. The lock is released when the process holding it exits, even when
# that process is killed, so it cannot go stale. A timeout is not treated as an
# error because the other process may already have fetched the submodule.
function(rp6502_submodule_fetch super path sparse)
    set(_lock ${super}/.git/rp6502-fetch.lock)
    if(IS_DIRECTORY ${super}/.git)
        file(LOCK ${_lock} GUARD PROCESS TIMEOUT 900 RESULT_VARIABLE _ignored)
    endif()
    message(STATUS "Fetching ${path}")
    if(sparse)
        rp6502_submodule_sparse(${super} ${path} ${sparse})
    else()
        set(_depth "")
        if(NOT DEFINED RP6502_SUBMODULE_DEPTH)
            set(RP6502_SUBMODULE_DEPTH "1")
        endif()
        if(NOT RP6502_SUBMODULE_DEPTH STREQUAL "0")
            set(_depth --depth ${RP6502_SUBMODULE_DEPTH})
        endif()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} -C ${super} submodule update --init
                ${_depth} -- ${path}
            RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
        if(_rc AND _depth)
            execute_process(
                COMMAND ${GIT_EXECUTABLE} -C ${super} submodule update --init
                    -- ${path}
                RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
        endif()
    endif()
endfunction()

function(rp6502_submodule_sparse super path sparse)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C ${super} rev-parse HEAD:${path}
        OUTPUT_VARIABLE _pin OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc ERROR_QUIET)
    if(_rc)
        return()
    endif()
    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C ${super} config --get
            submodule.${path}.url
        OUTPUT_VARIABLE _url OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _url)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} config -f ${super}/.gitmodules --get
                submodule.${path}.url
            OUTPUT_VARIABLE _url OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(NOT _url)
        return()
    endif()
    if(NOT EXISTS ${super}/${path}/.git)
        file(MAKE_DIRECTORY ${super}/${path})
        execute_process(COMMAND ${GIT_EXECUTABLE} clone --filter=blob:none
            --no-checkout ${_url} ${super}/${path} RESULT_VARIABLE _rc)
        if(_rc)
            return()
        endif()
    endif()
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${super}/${path}
        sparse-checkout set --no-cone ${sparse})
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${super}/${path}
        checkout ${_pin})
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND FETCH_PATH)
    find_package(Git REQUIRED)
    rp6502_submodule_fetch(${FETCH_SUPER} ${FETCH_PATH} "${FETCH_SPARSE}")
endif()
