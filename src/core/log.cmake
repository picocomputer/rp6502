# The log level a build carries, as compile definitions; core/sys/debug_log.h
# says what the macros do with them.
#
# RP6502_LOG_LEVEL   NONE, ERROR, WARN, INFO or DEBUG; empty takes the build
#                    type's own, ERROR in Debug and NONE otherwise.
# RP6502_LOG_LEVELS  category=level words, each raising or silencing one
#                    category: "ntp=DEBUG;usb=INFO".

include_guard(GLOBAL)

set(RP6502_LOG_LEVEL "" CACHE STRING
    "What every log category says: NONE, ERROR, WARN, INFO, DEBUG; empty for the build type's own")
set(RP6502_LOG_LEVELS "" CACHE STRING
    "What one category says, as category=level words")

set(RP6502_LOG_LEVEL_NAMES NONE ERROR WARN INFO DEBUG)

function(rp6502_log_level_check name where)
    if(NOT name IN_LIST RP6502_LOG_LEVEL_NAMES)
        message(FATAL_ERROR "${where}: '${name}' is not one of ${RP6502_LOG_LEVEL_NAMES}")
    endif()
endfunction()

# The definitions as a list of -D flags, for a compiler run outside a target.
function(rp6502_log_flags out)
    set(_flags)
    if(RP6502_LOG_LEVEL)
        string(TOUPPER ${RP6502_LOG_LEVEL} _level)
        rp6502_log_level_check(${_level} RP6502_LOG_LEVEL)
        list(APPEND _flags -DRP6502_LOG_LEVEL=RP6502_LOG_${_level})
    else()
        list(APPEND _flags -DRP6502_LOG_LEVEL=$<IF:$<CONFIG:Debug>,RP6502_LOG_ERROR,RP6502_LOG_NONE>)
    endif()
    foreach(_word IN LISTS RP6502_LOG_LEVELS)
        if(NOT _word MATCHES "^([A-Za-z0-9_]+)=([A-Za-z]+)$")
            message(FATAL_ERROR "RP6502_LOG_LEVELS: '${_word}' is not category=level")
        endif()
        set(_category ${CMAKE_MATCH_1})
        string(TOUPPER ${CMAKE_MATCH_2} _level)
        rp6502_log_level_check(${_level} RP6502_LOG_LEVELS)
        list(APPEND _flags -DRP6502_LOG_LEVEL_${_category}=RP6502_LOG_${_level})
    endforeach()
    set(${out} ${_flags} PARENT_SCOPE)
endfunction()

function(rp6502_log_definitions target scope)
    rp6502_log_flags(_flags)
    string(REPLACE "-D" "" _defs "${_flags}")
    target_compile_definitions(${target} ${scope} ${_defs})
endfunction()

# What the tinyusb category was set to, or empty, so a root can hand TinyUSB's
# own LOG the same answer.
function(rp6502_log_level_of category out)
    set(_level "")
    if(RP6502_LOG_LEVEL)
        string(TOUPPER ${RP6502_LOG_LEVEL} _level)
    endif()
    foreach(_word IN LISTS RP6502_LOG_LEVELS)
        if(_word MATCHES "^${category}=([A-Za-z]+)$")
            string(TOUPPER ${CMAKE_MATCH_1} _level)
        endif()
    endforeach()
    set(${out} ${_level} PARENT_SCOPE)
endfunction()
