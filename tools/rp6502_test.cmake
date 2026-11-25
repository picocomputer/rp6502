# Runs one command of a ctest run, for rp6502_test() in rp6502.cmake:
#
#   cmake -D RP6502_LOCK=<folder> [-D RP6502_SAVE_DIR=<folder>]
#         -P rp6502_test.cmake -- <command> [<arg>...]
#
# The command runs while this script holds the lock of RP6502_LOCK, so two
# runs of one test, or two builds, never run at once. The second one waits
# for the first. RP6502_SAVE_DIR is removed first, so a test starts with no
# saves. The output of the command passes through. Its exit code is the exit
# code of this script, or 1 for a failure with CMake before 3.29.

cmake_minimum_required(VERSION 3.28)

set(command)
set(found FALSE)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(i RANGE ${last})
    if(found)
        list(APPEND command "${CMAKE_ARGV${i}}")
    elseif(CMAKE_ARGV${i} STREQUAL "--")
        set(found TRUE)
    endif()
endforeach()

file(LOCK "${RP6502_LOCK}" DIRECTORY GUARD PROCESS RESULT_VARIABLE busy TIMEOUT 0)
if(NOT busy STREQUAL "0")
    message(STATUS "Waiting for another run to finish with ${RP6502_LOCK}")
    file(LOCK "${RP6502_LOCK}" DIRECTORY GUARD PROCESS)
endif()
if(DEFINED RP6502_SAVE_DIR)
    file(REMOVE_RECURSE "${RP6502_SAVE_DIR}")
endif()
execute_process(COMMAND ${command} RESULT_VARIABLE code)
if(code STREQUAL "0")
    return()
endif()
# The text of a signal, or of a command that did not start.
if(NOT code MATCHES "^[0-9]+$")
    message(FATAL_ERROR "${code}")
endif()
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    cmake_language(EXIT ${code})
endif()
message(FATAL_ERROR "Exit code ${code}")
