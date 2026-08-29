# The Pocket UI prints "Version " itself, so this takes the bare form that
# version.cmake's stamp script decides. Analogue caps the field at 31 chars.
include("${STAMP_SCRIPT}")
set(_ver "${_stamp_bare}")
string(LENGTH "${_ver}" _len)
if(_len GREATER 31)
    message(FATAL_ERROR "metadata.version over 31 chars: '${_ver}'")
endif()
string(TIMESTAMP _date "%Y-%m-%d" UTC)
file(READ "${CORE_JSON}" _json)
foreach(_key version date_release)
    string(REGEX MATCHALL "\"${_key}\"[ \t]*:" _m "${_json}")
    list(LENGTH _m _n)
    if(NOT _n EQUAL 1)
        message(FATAL_ERROR "${CORE_JSON}: expected one \"${_key}\", found ${_n}")
    endif()
endforeach()
string(REGEX REPLACE "(\"version\"[ \t]*:[ \t]*)\"[^\"]*\"" "\\1\"${_ver}\""
    _json "${_json}")
string(REGEX REPLACE "(\"date_release\"[ \t]*:[ \t]*)\"[^\"]*\"" "\\1\"${_date}\""
    _json "${_json}")
file(WRITE "${CORE_JSON}" "${_json}")
