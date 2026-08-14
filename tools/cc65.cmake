cmake_minimum_required(VERSION 3.20)

# Defines added so IntelliSense can parse cc65; cl65 must not see them.
set(CC65_INTELLISENSE_ONLY_DEFINES __fastcall__ __cdecl__)

# Skip toolchain config when invoked as a `cmake -P` wrapper script.
if(NOT CMAKE_SCRIPT_MODE_FILE)

# No default: a missing target reaches cl65 as a bare --target and fails
# somewhere much less helpful than here.
if(NOT CC65_TARGET_SYSTEM)
    message(FATAL_ERROR
        "cc65: CC65_TARGET_SYSTEM is not set. Set it before find_package(cc65).")
endif()

# Find the executables we'll be using.
set(CMAKE_SYSTEM_NAME Generic)
find_program(CMAKE_C_COMPILER cl65 REQUIRED)
find_program(CMAKE_ASM_COMPILER cl65 REQUIRED)
find_program(CMAKE_LINKER ld65 REQUIRED)
find_program(CMAKE_AR ar65 REQUIRED)
set(CC65_C_COMPILER "${CMAKE_C_COMPILER}" CACHE FILEPATH "Real cc65 C compiler")

# Query cc65 for the target define (e.g. __RP6502__).
file(WRITE "${CMAKE_BINARY_DIR}/_cc65_detect.c" "")
execute_process(
    COMMAND ${CC65_C_COMPILER} -Wc -dP -t ${CC65_TARGET_SYSTEM}
            -E -o "${CMAKE_BINARY_DIR}/_cc65_detect.i"
            "${CMAKE_BINARY_DIR}/_cc65_detect.c"
    ERROR_QUIET
)
file(READ "${CMAKE_BINARY_DIR}/_cc65_detect.i" CC65_DEFINE_TARGET)
file(REMOVE "${CMAKE_BINARY_DIR}/_cc65_detect.c" "${CMAKE_BINARY_DIR}/_cc65_detect.i")
string(REGEX MATCH "^#define (__[A-Z0-9_]+__) 1" CC65_DEFINE_TARGET "${CC65_DEFINE_TARGET}")
set(CC65_DEFINE_TARGET "${CMAKE_MATCH_1}")
if(NOT CC65_DEFINE_TARGET)
    message(WARNING "cc65: could not detect target define for ${CC65_TARGET_SYSTEM}")
endif()

# Add system include dir for analysis tools like IntelliSense.
execute_process(
    COMMAND ${CC65_C_COMPILER} --print-target-path
    OUTPUT_VARIABLE CC65_SYSTEM_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
cmake_path(APPEND CC65_SYSTEM_INCLUDE_DIR ".." "include")
cmake_path(ABSOLUTE_PATH CC65_SYSTEM_INCLUDE_DIR NORMALIZE)
include_directories(BEFORE SYSTEM ${CC65_SYSTEM_INCLUDE_DIR})

# Evil hack to get IntelliSense and problem matchers working by wrapping cl65.
# Comment out these lines to completely disable hack.
foreach(NAME IN LISTS CC65_INTELLISENSE_ONLY_DEFINES)
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:SHELL:-D${NAME}=>")
endforeach()
if(CC65_DEFINE_TARGET)
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:SHELL:-D${CC65_DEFINE_TARGET}=>")
endif()

set(CMAKE_C_COMPILER ${CMAKE_COMMAND})
set(CMAKE_C_COMPILER_ARG1 "-P ${CMAKE_CURRENT_LIST_FILE} -- ${CC65_C_COMPILER}")
set(CC65_ASM_COMPILER "${CMAKE_ASM_COMPILER}" CACHE FILEPATH "Real cc65 ASM compiler")
set(CMAKE_ASM_COMPILER ${CMAKE_COMMAND})
set(CMAKE_ASM_COMPILER_ARG1 "-P ${CMAKE_CURRENT_LIST_FILE} -- ${CC65_ASM_COMPILER}")

# Set C internals to work with cc65.
set(CMAKE_C_COMPILER_ID "cc65" CACHE STRING "C compiler ID")
set(CMAKE_C_DEPFILE_FORMAT gcc)
set(CMAKE_C_DEPENDS_USE_COMPILER TRUE)
set(CMAKE_DEPFILE_FLAGS_C "--create-dep <DEP_FILE>")
set(CMAKE_C_COMPILE_OBJECT "<CMAKE_C_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -o <OBJECT> --add-source -l <OBJECT>.s -c <SOURCE>")
set(CMAKE_C_CREATE_STATIC_LIBRARY "<CMAKE_AR> a <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_FLAGS "--target ${CC65_TARGET_SYSTEM}" CACHE STRING "cc65 C flags")
set(CMAKE_C_FLAGS_DEBUG "-g")
set(CMAKE_C_FLAGS_RELEASE "-Oirs")
set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> -m <TARGET>.map -Wl --dbgfile,<TARGET>.dbg <LINK_LIBRARIES>")
set(CMAKE_C_COMPILER_FORCED TRUE)

# cc65 has no C++. The language is enabled anyway so one project() line serves
# both compilers, and a C++ source that reaches this compiler is the error.
set(CMAKE_CXX_COMPILER ${CMAKE_COMMAND})
set(CMAKE_CXX_COMPILER_ARG1 "-P ${CMAKE_CURRENT_LIST_FILE} -- --no-cxx")
set(CMAKE_CXX_COMPILER_ID "cc65" CACHE STRING "CXX compiler ID")
set(CMAKE_CXX_COMPILE_OBJECT "<CMAKE_CXX_COMPILER> <SOURCE>")
set(CMAKE_CXX_OUTPUT_EXTENSION .o)
set(CMAKE_CXX_COMPILER_FORCED TRUE)

# Set ASM internals to work with cc65.
set(CMAKE_ASM_COMPILER_ID "cc65" CACHE STRING "ASM compiler ID")
set(CMAKE_INCLUDE_FLAG_ASM "--asm-include-dir ")
set(CMAKE_ASM_DEPFILE_FORMAT gcc)
set(CMAKE_ASM_DEPENDS_USE_COMPILER TRUE)
set(CMAKE_DEPFILE_FLAGS_ASM "--create-dep <DEP_FILE>")
set(CMAKE_ASM_COMPILE_OBJECT "<CMAKE_ASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -o <OBJECT> -c <SOURCE>")
set(CMAKE_ASM_CREATE_STATIC_LIBRARY ${CMAKE_C_CREATE_STATIC_LIBRARY})
set(CMAKE_ASM_FLAGS "--target ${CC65_TARGET_SYSTEM}" CACHE STRING "cc65 ASM flags")
set(CMAKE_ASM_FLAGS_DEBUG "-g")
set(CMAKE_ASM_FLAGS_RELEASE "")
set(CMAKE_ASM_SOURCE_FILE_EXTENSIONS s;asm;a65)
set(CMAKE_ASM_OUTPUT_EXTENSION .o)
set(CMAKE_ASM_LINK_EXECUTABLE "<CMAKE_ASM_COMPILER> <FLAGS> <CMAKE_ASM_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> -m <TARGET>.map -Wl --dbgfile,<TARGET>.dbg <LINK_LIBRARIES>")
set(CMAKE_ASM_LINKER_PREFERENCE 0)
set(CMAKE_ASM_LINKER_PREFERENCE_PROPAGATES 0)
set(CMAKE_ASM_INFORMATION_LOADED 1)
set(CMAKE_ASM_COMPILER_FORCED TRUE)

return()
endif() # End toolchain config

# Wrapper mode for IntelliSense and problem matchers.
# Args 0-3 are the cmake call to this script.
if(NOT CMAKE_ARGV3 STREQUAL "--")
    message(FATAL_ERROR "No -- separator found in arguments")
endif()

# First argument after -- is the real compiler.
set(CC65_COMPILER "${CMAKE_ARGV4}")

if(CC65_COMPILER STREQUAL "--no-cxx")
    message(FATAL_ERROR "cc65 has no C++ compiler: ${CMAKE_ARGV5}")
endif()

set(SKIP_ARGS "")
foreach(NAME IN LISTS CC65_INTELLISENSE_ONLY_DEFINES)
    list(APPEND SKIP_ARGS "-D${NAME}=")
endforeach()

# Remove defines intended for IntelliSense.
set(FILTERED_ARGS "")
foreach(INDEX RANGE 5 ${CMAKE_ARGC})
    if(DEFINED CMAKE_ARGV${INDEX})
        set(ARG "${CMAKE_ARGV${INDEX}}")
        if(NOT ARG IN_LIST SKIP_ARGS)
            list(APPEND FILTERED_ARGS "${ARG}")
        endif()
    endif()
endforeach()

# Execute the real compiler with filtered arguments.
execute_process(
    COMMAND ${CC65_COMPILER} ${FILTERED_ARGS}
    OUTPUT_VARIABLE STDOUT_OUTPUT
    ERROR_VARIABLE STDERR_OUTPUT
    RESULT_VARIABLE EXIT_CODE
)

if(STDOUT_OUTPUT)
    message(STATUS "${STDOUT_OUTPUT}")
endif()

if(STDERR_OUTPUT)
    # Reformat stderr so VS Code problem matcher works. Just a case change.
    string(REGEX REPLACE "(:[0-9]+:) Error:" "\\1 error:" STDERR_OUTPUT "${STDERR_OUTPUT}")
    string(REGEX REPLACE "(:[0-9]+:) Warning:" "\\1 warning:" STDERR_OUTPUT "${STDERR_OUTPUT}")
    message(NOTICE "${STDERR_OUTPUT}")
endif()

if(NOT EXIT_CODE EQUAL 0)
    message(FATAL_ERROR "Compilation failed with exit code ${EXIT_CODE}")
endif()
