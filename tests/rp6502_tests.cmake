# Shared CTest wiring. tests/cpu and tests/host/emu ride in a host's build,
# tests/rtl is a root of its own that adds tests/cpu too; this file carries
# what all of them need.
#
# The harness is utest.h, from its own repository rather than out of sokol's
# test directory. The copy in there is a fork of an older one, and what it is
# missing is most of the complaints: --list-tests, the annotated assertions,
# UTEST_SKIP, and a C++ printer that can take a uint64_t. Taking it directly
# also means the simulation tree stops pulling an entire graphics library to
# get one header — sokol is the emulator's dependency, not the RTL's.

# Entered from both trees, so it asks rather than assuming the other
# already did.
include(${RP6502_ROOT}/submodules.cmake)
rp6502_submodule(vendor/utest SENTINEL utest.h
    WANTS "the test harness")

set(RP6502_UTEST_DIR ${RP6502_VENDOR}/utest)
set(RP6502_TESTS_DIR ${CMAKE_CURRENT_LIST_DIR})
set(RP6502_TEST_ROMS ${RP6502_TESTS_DIR}/roms)

# The harness both sides share: mut.h and the machine it binds to, emu_boot.h
# and the sys shims for the C tests, the tb_* benches for the RTL ones. Every
# test gets it on the include path, because which of them a test needs is not
# worth stating.
# The two host helpers only tests want, as a translation unit per host rather
# than one file forked down the middle.
if(WIN32)
    set(RP6502_TB_HOSTOS ${CMAKE_CURRENT_LIST_DIR}/bench/tb_hostos_win.c)
else()
    set(RP6502_TB_HOSTOS ${CMAKE_CURRENT_LIST_DIR}/bench/tb_hostos_posix.c)
endif()

set(RP6502_BENCH ${RP6502_TESTS_DIR}/bench)

# The keyboard layouts as a C table. No machine in a desktop tree compiles
# them -- the RIA firmware builds its own copy, the Pocket stages the .bin, and
# the emulator's keyboard arrives already translated. Only the layout suites
# want this form, to hold the image against the defs it came from.
include(${RP6502_SRC}/core/gen.cmake)
rp6502_gen_kbdlay(kbdlay)

# The video-mode corpus is generated, not committed. Every byte of it comes
# out of vidmodes.py, so a committed copy is only a second copy that can
# disagree with its generator. What stays in roms/ is the cc65-built programs,
# which have no generator and are reached by FIXTURE rather than from here.
set(RP6502_TEST_ROM_DIR ${CMAKE_BINARY_DIR}/roms)
set(RP6502_TEST_CORPUS ${RP6502_TEST_ROM_DIR})
if(NOT TARGET rp6502_test_corpus)
    set(RP6502_CORPUS_GEN ${RP6502_TEST_ROMS}/vidmodes.py)
    # It assembles and packages through tests/gen like every other ROM
    # generator, so a change there is a change to the corpus.
    set(RP6502_CORPUS_ASM
        ${RP6502_TESTS_DIR}/gen/rp6502_asm.py
        ${RP6502_TESTS_DIR}/gen/rp6502_rom.py)
    # A stamp rather than the names: listing them here would be the same
    # duplication in a different file, and it would be wrong within a month.
    # The manifest beside them is how a suite asks what it is looking at —
    # the geometry the emulator's suite used to carry as forty-seven pairs of
    # numbers, one of which was wrong.
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/roms.stamp
        COMMAND ${CMAKE_COMMAND} -E env python3
            ${RP6502_CORPUS_GEN} --out ${RP6502_TEST_CORPUS}
            --emit-manifest ${RP6502_TEST_CORPUS}/manifest.txt
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/roms.stamp
        DEPENDS ${RP6502_CORPUS_GEN} ${RP6502_CORPUS_ASM}
        COMMENT "Generating the video-mode ROM corpus"
        VERBATIM)
    add_custom_target(rp6502_test_corpus DEPENDS ${CMAKE_BINARY_DIR}/roms.stamp)
endif()

# --- The generated 6502 programs ---
#
# A .rp6502 is a program, not a package. Nothing verilates one and nothing
# links one, so unlike the assets in src/core/assets.cmake these need no
# configure-time copy; a build rule is enough. They are here because the
# machine that runs them is not the point of them: a program that writes a
# file, reads it back and prints what came back makes the same claim on a
# Pocket, on a Pico and in the emulator. While these rules belonged to the
# FPGA tree that was the only tree that could make it, and the whole drive
# conformance ROM ran nowhere else.

# One target the programs hang off. A C test reaches its ROM through
# add_dependencies on its executable; a script test has no executable to hang
# one on, so this is ALL.
if(NOT TARGET rp6502_test_roms)
    add_custom_target(rp6502_test_roms ALL)
    add_dependencies(rp6502_test_roms rp6502_test_corpus)
endif()

# rp6502_test_rom(<target> GEN <script> OUTPUTS <file>... [ARGS <arg>...]
#                 [DEPENDS <file>...] COMMENT <text>)
function(rp6502_test_rom target)
    cmake_parse_arguments(R "" "GEN;COMMENT" "OUTPUTS;ARGS;DEPENDS" ${ARGN})
    # The generator and its sources stand for the outputs in the input list —
    # a generated file never appears in a diff — and they go into the same
    # property rp6502_add_test appends to, because to the question "does this
    # commit change the simulation" a ROM the simulation boots and a test that
    # boots it are one answer. tests/rtl/CMakeLists.txt reads this.
    add_custom_command(OUTPUT ${R_OUTPUTS}
        COMMAND ${CMAKE_COMMAND} -E env python3 ${R_GEN} ${R_ARGS}
        DEPENDS ${R_GEN} ${R_DEPENDS}
        COMMENT ${R_COMMENT}
        VERBATIM)
    add_custom_target(${target} DEPENDS ${R_OUTPUTS})
    add_dependencies(rp6502_test_roms ${target})
endfunction()

# The assembler and the container every one of these generators writes
# through. A change to either changes every ROM, so every ROM names them.
set(RP6502_ROM_GEN
    ${RP6502_TESTS_DIR}/gen/rp6502_asm.py
    ${RP6502_TESTS_DIR}/gen/rp6502_rom.py)

# Two audio programs that make one note and leave the console alone, so
# the machine's own diagnostics stay readable while a device is driven.
# test_aud runs these same files, which is what keeps a note that sounds
# on hardware and a note the simulation asserts from drifting apart.
set(AUD_ROM_PSG ${RP6502_TEST_ROM_DIR}/psg.rp6502)
set(AUD_ROM_PSG_PRE ${RP6502_TEST_ROM_DIR}/psg_pre.rp6502)
set(AUD_ROM_OPL ${RP6502_TEST_ROM_DIR}/opl.rp6502)
set(AUD_ROM_OPL_EXIT ${RP6502_TEST_ROM_DIR}/opl_exit.rp6502)
set(AUD_ROM_OPL_INIT ${RP6502_TEST_ROM_DIR}/opl_init.rp6502)
set(AUD_ROM_BEL ${RP6502_TEST_ROM_DIR}/bel.rp6502)
set(AUD_ROM_OPL_BEL ${RP6502_TEST_ROM_DIR}/opl_bel.rp6502)
rp6502_test_rom(aud_roms GEN ${RP6502_TESTS_DIR}/gen/aud_rom_gen.py
    ARGS --emit-psg ${AUD_ROM_PSG} --emit-psg-pre ${AUD_ROM_PSG_PRE}
        --emit-opl ${AUD_ROM_OPL}
        --emit-opl-exit ${AUD_ROM_OPL_EXIT}
        --emit-opl-init ${AUD_ROM_OPL_INIT}
        --emit-bel ${AUD_ROM_BEL} --emit-opl-bel ${AUD_ROM_OPL_BEL}
    OUTPUTS ${AUD_ROM_PSG} ${AUD_ROM_PSG_PRE} ${AUD_ROM_OPL}
        ${AUD_ROM_OPL_EXIT} ${AUD_ROM_OPL_INIT}
        ${AUD_ROM_BEL} ${AUD_ROM_OPL_BEL}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the audio bring-up ROMs")

# The file round trip, generated the same way and shipped the same way.
# The file that is open when the machine sleeps. It reads a chunk at a
# time so that wherever a sleep lands, a read lands after the resume.
set(STREAM_ROM ${RP6502_TEST_ROM_DIR}/stream.rp6502)
rp6502_test_rom(stream_rom GEN ${RP6502_TESTS_DIR}/gen/stream_rom_gen.py
    ARGS --emit ${STREAM_ROM}
    OUTPUTS ${STREAM_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the streaming-read ROM")

set(FILE_ROM ${RP6502_TEST_ROM_DIR}/file.rp6502)
rp6502_test_rom(file_rom GEN ${RP6502_TESTS_DIR}/gen/file_rom_gen.py
    ARGS --emit ${FILE_ROM}
    OUTPUTS ${FILE_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the file round-trip ROM")

# The same round trip past the transfer window. It ships but is not a
# test: what it exists to ask — whether the Pocket's resize keeps what
# was already in the file — has no answer in simulation, because the
# bench answers the way we assumed.
set(BIGFILE_ROM ${RP6502_TEST_ROM_DIR}/bigfile.rp6502)
rp6502_test_rom(bigfile_rom GEN ${RP6502_TESTS_DIR}/gen/bigfile_rom_gen.py
    ARGS --emit ${BIGFILE_ROM}
    OUTPUTS ${BIGFILE_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the multi-chunk file ROM")

# The create path has never worked on hardware and the name turned out
# not to matter. This walks a list of names in one boot so the next
# guess costs a card copy instead of a fit.
set(PROBE_ROM ${RP6502_TEST_ROM_DIR}/probe.rp6502)
rp6502_test_rom(probe_rom GEN ${RP6502_TESTS_DIR}/gen/probe_rom_gen.py
    ARGS --emit ${PROBE_ROM}
    OUTPUTS ${PROBE_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the open-file probe ROM")

# A file read across a sleep, and a count printed while it is read:
# issue 183 in the reads and issue 185 in the gap the count shows. The
# Pocket bench runs it for the reads, which is where a second worker on
# the file bridge can answer them; the count needs a sleep, so that half
# is run on hardware.
set(SLEEPFILE_ROM ${RP6502_TEST_ROM_DIR}/sleepfile.rp6502)
rp6502_test_rom(sleepfile_rom GEN ${RP6502_TESTS_DIR}/gen/sleepfile_rom_gen.py
    ARGS --emit ${SLEEPFILE_ROM}
    OUTPUTS ${SLEEPFILE_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the sleep file probe ROM")

# Music's stand-in, in two variants. A ROM: asset read in a loop forever,
# printing what arrives, so a descriptor left on the wrong file after a
# sleep says so in letters rather than in silence. Built here so they
# keep compiling; they are read on a Pocket, not by ctest.
set(SLEEPASSET_A_ROM ${RP6502_TEST_ROM_DIR}/sleepasset-a.rp6502)
set(SLEEPASSET_B_ROM ${RP6502_TEST_ROM_DIR}/sleepasset-b.rp6502)
rp6502_test_rom(sleepasset_a_rom GEN ${RP6502_TESTS_DIR}/gen/sleepasset_rom_gen.py
    ARGS --emit ${SLEEPASSET_A_ROM} --variant A
    OUTPUTS ${SLEEPASSET_A_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the sleep asset probe ROM (A)")
rp6502_test_rom(sleepasset_b_rom GEN ${RP6502_TESTS_DIR}/gen/sleepasset_rom_gen.py
    ARGS --emit ${SLEEPASSET_B_ROM} --variant B
    OUTPUTS ${SLEEPASSET_B_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the sleep asset probe ROM (B)")

# The whole drive in one boot: forty-eight checks the machine decides
# for itself. It runs here against the bench's host as well as on the
# card, so a bug in the ROM is found before a photograph is.
# argv[0] read back by the program it names. On the Pocket that string
# can only have come from asking the host what the ROM slot is bound to,
# so an empty argv here is that ask being thrown away.
set(ARGV_ROM ${RP6502_TEST_ROM_DIR}/argv.rp6502)
rp6502_test_rom(argv_rom GEN ${RP6502_TESTS_DIR}/gen/argv_rom_gen.py
    ARGS --emit ${ARGV_ROM}
    OUTPUTS ${ARGV_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the argv ROM")

set(FSTEST_ROM ${RP6502_TEST_ROM_DIR}/fstest.rp6502)
rp6502_test_rom(fstest_rom GEN ${RP6502_TESTS_DIR}/gen/fstest_rom_gen.py
    ARGS --emit ${FSTEST_ROM}
    OUTPUTS ${FSTEST_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the filesystem conformance ROM")

# The console read as a device. A program can open TTY: and read it raw,
# with no line editor in between, and nothing was asking whether a byte
# typed at the machine arrives there -- the conformance ROM opens that
# descriptor and closes it without reading a byte through it.
set(TTY_ROM ${RP6502_TEST_ROM_DIR}/tty.rp6502)
rp6502_test_rom(tty_rom GEN ${RP6502_TESTS_DIR}/gen/tty_rom_gen.py
    ARGS --emit ${TTY_ROM}
    OUTPUTS ${TTY_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the raw console read ROM")

# rp6502_add_script_test(<name> [SCRIPT <file>] [ROM <file>]
#                        [FIXTURE <file in roms/>] [ARGS <emu arg>...]
#                        [DEPENDS <target>...] [TIMEOUT <seconds>])
#
# A 6502 program and a script that watches it, as one CTest. The program makes
# the claims it can make from inside the machine and prints them; the script
# drives the inputs no program can give itself and reads the answers back.
# That pairing is what a functional test of this machine is, and it existed
# only as hand-written add_test lines under host/emu, where no subsystem could
# reach it.
#
# No SPLIT, and for the opposite reason to rp6502_add_test's. There, splitting
# turns a suite bounded by its slowest binary into one bounded by its slowest
# case, because the binary is a nine-minute simulation. Here a boot costs
# milliseconds and the point is that it answers twenty questions; a case apiece
# would boot twenty times to save nothing.
function(rp6502_add_script_test name)
    cmake_parse_arguments(S "" "SCRIPT;DRIVER;ROM;FIXTURE;TIMEOUT" "ARGS;DEPENDS" ${ARGN})
    if(NOT TARGET rp6502-emu)
        message(FATAL_ERROR
            "rp6502_add_script_test(${name}) drives the shipped binary.\n"
            "  Guard the call with if(TARGET rp6502-emu).")
    endif()

    if(S_DRIVER)
        # The other shape: the file that assembles the program drives it too,
        # over the pipe script.c answers on. One file, one set of constants, and
        # a static script that could disagree with its program is not written.
        if(NOT IS_ABSOLUTE ${S_DRIVER})
            set(S_DRIVER ${CMAKE_CURRENT_LIST_DIR}/${S_DRIVER})
        endif()
        set(S_SCRIPT ${S_DRIVER})
    else()
        if(NOT S_SCRIPT)
            set(S_SCRIPT ${name}.txt)
        endif()
        if(NOT IS_ABSOLUTE ${S_SCRIPT})
            set(S_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/${S_SCRIPT})
        endif()
    endif()
    if(S_FIXTURE)
        set(S_ROM ${RP6502_TEST_ROMS}/${S_FIXTURE})
    endif()
    if(NOT S_ROM)
        message(FATAL_ERROR "rp6502_add_script_test(${name}) names no program")
    endif()
    # A directory of its own. A script test writes where it is standing — a
    # screenshot, and every file the program creates on a host-backed drive —
    # and two of them in one directory under ctest --parallel is how a suite
    # starts failing on other people's machines.
    set(_work ${CMAKE_CURRENT_BINARY_DIR}/script.${name})
    file(MAKE_DIRECTORY ${_work})

    file(RELATIVE_PATH _dir ${RP6502_TESTS_DIR} ${CMAKE_CURRENT_LIST_DIR})
    string(REPLACE "/" "." _dir "${_dir}")

    # --mute so a runner opens no audio device, --seed and --fill so a program
    # that asks for entropy or reads memory it never wrote is asked the same
    # question every run.
    if(S_DRIVER)
        # The driver spawns the emulator itself, so it is told where both are.
        add_test(NAME script.${name}
            COMMAND ${CMAKE_COMMAND} -E env python3 ${S_DRIVER} --drive
                --emu $<TARGET_FILE:rp6502-emu> --rom ${S_ROM} ${S_ARGS})
    else()
        add_test(NAME script.${name}
            COMMAND rp6502-emu --mute --seed 1 --fill 0 ${S_ARGS}
                --script ${S_SCRIPT} ${S_ROM})
    endif()
    if(NOT S_TIMEOUT)
        set(S_TIMEOUT 120)
    endif()
    # EMU_ECHO mirrors the terminal to stderr (core/com/tty.c). A script fails on
    # one line and the question is always what the machine had been saying, so
    # --output-on-failure carries the console with it.
    set_tests_properties(script.${name} PROPERTIES
        WORKING_DIRECTORY ${_work}
        TIMEOUT ${S_TIMEOUT}
        ENVIRONMENT EMU_ECHO=1
        LABELS "script;${_dir}")

    # add_test cannot depend on a target, and a script test has no executable
    # to hang one on, so a ROM it needs is hung off the aggregate instead.
    if(S_DEPENDS)
        add_dependencies(rp6502_test_roms ${S_DEPENDS})
    endif()
endfunction()

# rp6502_add_test(<name> [SOURCES ...] [LIBS ...] [INCLUDES ...] [DEFS ...]
#                        [FIXTURE <file in roms/>] [TIMEOUT <seconds>] [SPLIT]
#                        [DEPENDS <target> ...])
#
# Builds test_<name> from test_<name>.c unless SOURCES says otherwise, and
# registers it as CTest <name>. FIXTURE becomes TEST_FIXTURE, the absolute path
# a test opens — every test uses at most one. TEST_SCRATCH is where a test
# writes throwaway files, so a run from any directory never litters the tree.
# DEPENDS names a target the test needs built but does not link, which is what
# a suite that opens the artifact rather than its objects has instead of a
# link line.
#
# SPLIT registers each UTEST case as its own CTest test instead. For the long
# ones that is the difference between a suite bounded by its slowest binary and
# one bounded by its slowest case; for the forty that finish in a fifth of a
# second it would cost more in process starts than it saves, which is why it is
# asked for rather than assumed.
function(rp6502_add_test name)
    cmake_parse_arguments(T "SPLIT" "FIXTURE;TIMEOUT"
        "SOURCES;LIBS;INCLUDES;DEFS;LABELS;DEPENDS" ${ARGN})

    # What a test is about is the directory it lives in, and what it costs is
    # which helper registered it. Both are already known here, so neither is
    # asked for: a LABELS argument at every call site would be a third copy of
    # two things the build can see, and the first to go stale would be the one
    # nobody reruns. CMAKE_CURRENT_LIST_DIR inside a function is the caller's,
    # which is the same reason relative SOURCES resolve.
    file(RELATIVE_PATH _dir ${RP6502_TESTS_DIR} ${CMAKE_CURRENT_LIST_DIR})
    string(REPLACE "/" "." _dir "${_dir}")
    if(NOT T_LABELS)
        set(T_LABELS c)
    endif()
    list(APPEND T_LABELS ${_dir})

    if(NOT T_SOURCES)
        set(T_SOURCES test_${name}.c)
    endif()

    # The bench's own answers to host/host.h, which every machine owes.
    list(APPEND T_SOURCES ${RP6502_BENCH}/tb_seed.c ${RP6502_SRC}/core/sys/crc32.c)

    add_executable(test_${name} ${T_SOURCES})
    target_include_directories(test_${name} PRIVATE
        ${RP6502_UTEST_DIR} ${RP6502_BENCH} ${RP6502_SRC} ${T_INCLUDES})

    if(T_LIBS)
        target_link_libraries(test_${name} PRIVATE ${T_LIBS})
    endif()
    list(APPEND T_DEFS TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}")
    # A bare name is one of the committed programs in roms/; a name with a
    # slash is relative to tests/, for fixtures that are not .rp6502 at all
    # and belong beside the suite that reads them.
    if(T_FIXTURE MATCHES "/")
        list(APPEND T_DEFS TEST_FIXTURE="${RP6502_TESTS_DIR}/${T_FIXTURE}")
    elseif(T_FIXTURE)
        list(APPEND T_DEFS TEST_FIXTURE="${RP6502_TEST_ROMS}/${T_FIXTURE}")
    endif()
    if(T_DEFS)
        target_compile_definitions(test_${name} PRIVATE ${T_DEFS})
    endif()

    add_dependencies(test_${name} rp6502_test_corpus ${T_DEPENDS})

    set(_cases ${name})
    if(T_SPLIT)
        # The cases are read out of the source. Every one of them is a plain
        # one-line UTEST(), and the file is a configure dependency, so adding a
        # case is enough to register it.
        #
        # utest.h can list them itself now, which would be the more honest
        # question to ask — it would see a case behind an #if, and this cannot.
        # But asking means running the binary, so registration moves from
        # configure time to build time and a per-case TIMEOUT gets harder. The
        # four suites that split have no conditional cases, and the two answers
        # agree today; when they stop agreeing, --list-tests is the way out.
        list(GET T_SOURCES 0 _src)
        if(NOT IS_ABSOLUTE ${_src})
            set(_src ${CMAKE_CURRENT_LIST_DIR}/${_src})
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_src})
        set(_cases)
        file(STRINGS ${_src} _decls REGEX "^UTEST\\(")
        foreach(_decl IN LISTS _decls)
            string(REGEX REPLACE "^UTEST\\( *([A-Za-z0-9_]+) *, *([A-Za-z0-9_]+).*"
                "\\1.\\2" _case "${_decl}")
            list(APPEND _cases ${_case})
        endforeach()
        if(NOT _cases)
            message(FATAL_ERROR "${_src} declares no UTEST cases to split")
        endif()
    endif()

    foreach(_case IN LISTS _cases)
        set(_filter)
        if(T_SPLIT)
            set(_filter --filter=${_case})
        endif()
        add_test(NAME ${_case} COMMAND test_${name} ${_filter})
        set_tests_properties(${_case} PROPERTIES LABELS "${T_LABELS}")
        if(T_TIMEOUT)
            set_tests_properties(${_case} PROPERTIES TIMEOUT ${T_TIMEOUT})
        endif()
    endforeach()
    if(T_SPLIT)
        # A filter that matches nothing runs nothing and exits zero, which is a
        # green test that never ran.
        set_tests_properties(${_cases} PROPERTIES
            FAIL_REGULAR_EXPRESSION "Running 0 test cases")
    endif()
endfunction()
