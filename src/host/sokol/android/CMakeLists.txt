# The emulator on Android: the native library, and the APK around it.
#
# Two ways in. Gradle configures this file through AGP's externalNativeBuild,
# passing its own toolchain, ABI and platform; that build packages and signs.
# A standalone `cmake -S src/host/sokol/android -B build/android/cmake` finds
# the same pinned NDK itself, which is what gives an editor a
# compile_commands.json and what lets the library be built without Gradle at
# all.
#
# The pins live in gradle.properties, which Gradle reads as project properties
# and this file reads as text, so the NDK and the API level are written once.

cmake_minimum_required(VERSION 3.21)

# The Gradle project. A manifest, a wrapper and an SDK pin name this
# application rather than the system under it, so it sits beside the machine's
# own C. Named before project() because the pins below are read out of it.
set(RP6502_APK ${CMAKE_CURRENT_LIST_DIR}/apk)

# Read the pins before project(): the toolchain has to be chosen first.
set(RP6502_GRADLE_PROPERTIES ${RP6502_APK}/gradle.properties)
function(rp6502_gradle_property key out)
    file(STRINGS ${RP6502_GRADLE_PROPERTIES} _hits REGEX "^${key} *= *")
    list(LENGTH _hits _n)
    if(NOT _n EQUAL 1)
        message(FATAL_ERROR
            "gradle.properties: expected one ${key}, found ${_n}")
    endif()
    string(REGEX REPLACE "^${key} *= *" "" _val "${_hits}")
    string(STRIP "${_val}" _val)
    if(NOT _val)
        message(FATAL_ERROR "gradle.properties: ${key} is empty")
    endif()
    set(${out} "${_val}" PARENT_SCOPE)
endfunction()
rp6502_gradle_property(rpNdkVersion RP6502_NDK_VERSION)
rp6502_gradle_property(rpMinSdk RP6502_MIN_SDK)

# AGP always passes a toolchain, so this is the standalone path. The SDK is
# found the way AGP finds it — ANDROID_HOME, ANDROID_SDK_ROOT, then
# local.properties' sdk.dir — and the NDK by the pinned version under it, so
# the two ways in cannot compile against different NDKs.
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(_sdk "$ENV{ANDROID_HOME}")
    if(NOT _sdk)
        set(_sdk "$ENV{ANDROID_SDK_ROOT}")
    endif()
    if(NOT _sdk AND EXISTS ${RP6502_APK}/local.properties)
        file(STRINGS ${RP6502_APK}/local.properties _line
            REGEX "^sdk\\.dir *= *")
        string(REGEX REPLACE "^sdk\\.dir *= *" "" _sdk "${_line}")
        string(REPLACE "\\:" ":" _sdk "${_sdk}")
        string(REPLACE "\\\\" "/" _sdk "${_sdk}")
    endif()
    set(_ndk "${_sdk}/ndk/${RP6502_NDK_VERSION}")
    if(NOT _sdk OR NOT EXISTS ${_ndk}/build/cmake/android.toolchain.cmake)
        message(FATAL_ERROR
            "the Android NDK ${RP6502_NDK_VERSION} is absent.\n"
            "  Set ANDROID_HOME, then:\n"
            "  sdkmanager \"ndk;${RP6502_NDK_VERSION}\"")
    endif()
    set(CMAKE_TOOLCHAIN_FILE ${_ndk}/build/cmake/android.toolchain.cmake)
    set(ANDROID_ABI arm64-v8a)
    set(ANDROID_PLATFORM android-${RP6502_MIN_SDK})
endif()

project(rp6502-emu-android C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Emit compile_commands.json

cmake_path(SET RP6502_ROOT NORMALIZE ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(RP6502_SRC ${RP6502_ROOT}/src)
set(RP6502_VENDOR ${RP6502_ROOT}/vendor)

# First: it takes RP6502_CI out of the cache, so nothing may read it earlier.
include(${RP6502_ROOT}/version.cmake)

set(RP6502_OSAL ${RP6502_SRC}/osal/android)
include(${RP6502_SRC}/core/emu.cmake)
# Bionic has no <aio.h>, and the shared seam has a synchronous transport for
# exactly that reason — the libretro core takes the same pair on every OS
# that is not Windows. One bionic filesystem answer, not two.
include(${RP6502_SRC}/osal/posix/posix.cmake)
rp6502_osal_posix(emu_core TRANSPORT sync)
target_sources(emu_core PRIVATE ${RP6502_OSAL}/os.c)

# The machine this is: Android is a host under host/sokol, not a machine of
# its own, so it takes the drivers.h beside it rather than a copy.
target_include_directories(emu_core PUBLIC ${RP6502_SRC}/host/sokol)


include(${RP6502_SRC}/host/sokol/sokol.cmake)

# No main() and no SOKOL_NO_ENTRY: this host enters through sokol_main in
# entry.c, which NativeActivity calls.
add_library(rp6502-emu SHARED ${RP6502_EMU_APP} ${RP6502_EMU_WINDOW}
    ${CMAKE_CURRENT_LIST_DIR}/entry.c
    ${CMAKE_CURRENT_LIST_DIR}/menu.c)
target_link_libraries(rp6502-emu PRIVATE emu_core android log EGL GLESv3 aaudio)
target_compile_definitions(rp6502-emu PRIVATE SOKOL_GLES3 RP6502_ANDROID_INPUT_HOOK)

rp6502_use_version_header(rp6502-emu ${RP6502_SRC}/core/sys/version.c emu_core)

# The APK, for the standalone tree only: under AGP this same file is being
# read by the Gradle build that would be invoked, and RP6502_GRADLE is how it
# says so.
if(NOT RP6502_GRADLE)
    set(_gradlew ${RP6502_APK}/gradlew)
    if(CMAKE_HOST_WIN32)
        set(_gradlew ${RP6502_APK}/gradlew.bat)
    endif()
    add_custom_target(apk
        COMMAND ${_gradlew} assembleRelease
            -PrpVersion=${RP6502_VERSION_VALUE} -PrpCi=${RP6502_CI_VALUE}
        WORKING_DIRECTORY ${RP6502_APK}
        COMMENT "Building the APK (Gradle compiles this library again, its way)"
        USES_TERMINAL
        VERBATIM)
endif()
