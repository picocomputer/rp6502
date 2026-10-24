#[=======================================================================[.rst:
rp6502
------

The CMake tools of a Picocomputer 6502 project. ``CMakeLists.txt``
includes this file before ``project()``:

.. code-block:: cmake

  include(${CMAKE_CURRENT_LIST_DIR}/tools/rp6502.cmake)
  project(hello C ASM)

The include loads the compiler for the variable that the configure preset
sets: cc65 for ``CC65_TARGET_SYSTEM``, llvm-mos for ``LLVM_MOS_PLATFORM``,
or none for ``RP6502_BASIC``, a project of BASIC programs only. The include
also adds BASIC as a language for ``project()``, enables testing, and sets
``RP6502_TOOLS_DIR`` to the ``tools/`` folder. With testing enabled, no
target can have the name ``test``.

Commands
^^^^^^^^

* :command:`rp6502_align` checks that addresses are even.
* :command:`rp6502_asset` adds a file to a ROM.
* :command:`rp6502_byproduct` adds a byproduct of the link to a ROM.
* :command:`rp6502_executable` packages the linker output as a ROM.
* :command:`rp6502_basic` packages BASIC programs as a ROM.
* :command:`rp6502_web` packages a ROM as a web page.
* :command:`rp6502_test` adds a test that runs in the emulator.

Functions named ``_rp6502_*`` are internal.

Tool Versions
^^^^^^^^^^^^^

The native emulator, the web player and BASIC come from the releases of
``picocomputer/rp6502`` and ``picocomputer/msbasic``. Configure fetches each
of them once into ``tools/`` under the file name of its release, such as
``tools/rp6502-0.40-web.zip``, and unpacks it into ``build/tools/``. Three
settings select the versions:

``RP6502_EMU_VERSION``
  The native emulator of each executable target, BASIC target and test.

``RP6502_WEB_VERSION``
  The web player of each :command:`rp6502_web` target.

``RP6502_BASIC_VERSION``
  The BASIC of each :command:`rp6502_basic` target.

A version starts with a digit, such as ``0.40``. The tag of its release is
``v0.40``. Without a version, a target or a test gets the newest release in
``tools/``, and configure fetches the latest release when ``tools/`` has
none.

The value of a setting comes from the cache, which ``-D`` and presets set.
Without a cache value, it comes from the property of the target or the
test. Without a property, it comes from the variable in the folder of the
target, or in the folder that calls :command:`rp6502_test`. A variable
applies to that folder and to the folders that it adds after it is set, as
any CMake variable does.

.. code-block:: cmake

  set(RP6502_EMU_VERSION 0.40)
  set_target_properties(paint PROPERTIES RP6502_EMU_VERSION 0.39)
  set_tests_properties(picker PROPERTIES RP6502_EMU_VERSION 0.39)

``RP6502_EMU``, ``RP6502_WEB`` and ``RP6502_BASIC`` are settings of the
same kind. Each is the path of a file that replaces the emulator, the web
player or BASIC of a release: an emulator executable, the zip of a web
player, or a ``basic.rp6502``. A file found at any level comes before any
version. A relative path is a path from the top source folder.
``RP6502_BASIC`` is also the switch of a BASIC project, so a CMake true
value, such as ``ON``, is not a file path.

Each target folder and each test folder has ``rp6502-emu``
(``rp6502-emu.exe`` on Windows), a link to the emulator of the target or
the test.

Update
^^^^^^

``cmake -P tools/rp6502.cmake`` replaces each file that ``tools/SHA256SUMS``
of ``RP6502_TOOLS_REPO`` lists, after a check of the hash. It then fetches
the latest release of each of the emulator for the host, the web player
and BASIC that ``tools/`` has. It also updates ``.vscode/tasks.json``,
``.vscode/launch.json``, ``.gitignore`` and the ``rp6502_map()`` calls of
``CMakeLists.txt`` when those files exist. Commit the scripts. Git ignores
the emulator, the web player and BASIC.
#]=======================================================================]

# Ubuntu 24.04 has CMake 3.28. Do not require a later version.
cmake_minimum_required(VERSION 3.28)

set(RP6502_TOOLS_REPO "picocomputer/rp6502")
set(RP6502_TOOLS_REF "main")
set(RP6502_BASIC_REPO "picocomputer/msbasic")

set(RP6502_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "RP6502 tools directory")
get_filename_component(RP6502_PROJECT_DIR "${RP6502_TOOLS_DIR}" DIRECTORY)

# The folder of cc65-config.cmake, for find_package(cc65).
set(cc65_DIR "${RP6502_TOOLS_DIR}")

#[=======================================================================[.rst:
.. command:: rp6502_align

  Checks that addresses are even.

  .. code-block:: cmake

    rp6502_align(<target> <regex>)

  The value of each symbol in ``<target>.sym`` whose whole name matches
  ``<regex>``, a Python regular expression, must be even and in the range
  $0-$FFFF. For each symbol that fails, the build of ``<target>`` reports
  an error at the ``#define`` of the symbol, or at its line in
  ``<target>.sym`` when the sources have no ``#define`` of it. A negative
  lookahead exempts names, as in ``"XRAM_(?!LOGO$).*"``. Each call adds
  one ``<regex>``. Call this before :command:`rp6502_executable`.
#]=======================================================================]
function(rp6502_align target)
    if (NOT ARGC EQUAL 2)
        message(FATAL_ERROR "rp6502_align(<target> <regex>)")
    endif()
    get_target_property(executable_called ${target} RP6502_EXECUTABLE_CALLED)
    if (executable_called)
        message(FATAL_ERROR
            "rp6502_align(${target} ...) must be registered BEFORE calling rp6502_executable()."
        )
    endif()
    set_property(TARGET ${target} APPEND PROPERTY RP6502_ALIGN "${ARGV1}")
endfunction()

# Migration: this stub converts rp6502_map() to rp6502_align() for older
# projects, and ignores the header.
function(rp6502_map target header regex)
    set(pattern "${regex}")
    if (ARGC EQUAL 4)
        set(pattern "(?!(?:${ARGV3})$)(?:${regex})")
    endif()
    # The escapes let the line in the warning paste into CMakeLists.txt.
    string(REPLACE "\\" "\\\\" shown "${pattern}")
    string(REPLACE "\"" "\\\"" shown "${shown}")
    message(WARNING
        "rp6502_map(${target} ${header} ...): change it to rp6502_align(${target} \"${shown}\").")
    rp6502_align(${target} "${pattern}")
endfunction()

#[=======================================================================[.rst:
.. command:: rp6502_asset

  Adds ``<file>`` to the ROM of ``<target>``.

  .. code-block:: cmake

    rp6502_asset(<target> <address> <file>)
    rp6502_asset(<target> RAM(<x>) <file>)
    rp6502_asset(<target> XRAM(<x>) <file>)

  A numeric ``<address>``, decimal or hex that starts with ``0x`` or ``$``,
  loads ``<file>`` into RAM ($0-$FFFF) or XRAM ($10000-$1FFFF) when the ROM
  loads. Any other ``<address>`` is a file name in the ROM, which a program
  opens as ``ROM:<address>``. A relative ``<file>`` is a path from the
  current source folder. For a generated file, such as the output of a
  custom command, ``<file>`` is the full path, as in
  ``${CMAKE_CURRENT_BINARY_DIR}/logo.bin``. For a file that the link writes,
  use :command:`rp6502_byproduct`. Call this before
  :command:`rp6502_executable` or :command:`rp6502_basic`.

  ``RAM(<x>)`` and ``XRAM(<x>)`` load ``<file>`` into RAM or XRAM at
  ``<x>``, which ``rp6502.py`` reads when the build packages the asset.
  ``<x>`` is a number in the same forms, or a symbol in ``<target>.sym``. A
  CMake variable goes in as ``${<variable>}``. With :command:`rp6502_basic`,
  ``<x>`` must be a number, because a BASIC ROM has no ``.sym`` file.
#]=======================================================================]
function(rp6502_asset target)
    get_target_property(executable_called ${target} RP6502_EXECUTABLE_CALLED)
    if (executable_called)
        message(FATAL_ERROR
            "rp6502_asset(${target} ...) must be registered BEFORE calling rp6502_executable() or rp6502_basic()."
        )
    endif()
    # CMake passes RAM(<x>) as the four arguments RAM, (, <x> and ).
    set(args ${ARGN})
    list(LENGTH args argc)
    list(GET args 0 addr)
    set(sym FALSE)
    if (addr STREQUAL "RAM" OR addr STREQUAL "XRAM")
        if (NOT argc EQUAL 5)
            message(FATAL_ERROR "rp6502_asset(${target} ${addr}(<address>) <in_file>)")
        endif()
        list(GET args 1 opened)
        list(GET args 2 token)
        list(GET args 3 closed)
        if (NOT opened STREQUAL "(" OR NOT closed STREQUAL ")")
            message(FATAL_ERROR "rp6502_asset(${target} ${addr}(<address>) <in_file>)")
        endif()
        # rp6502.py reads <x> as a number, or as a symbol in <target>.sym.
        set(addr "${addr}(${token})")
        set(sym TRUE)
        list(GET args 4 in_file)
    else()
        if (NOT argc EQUAL 2)
            message(FATAL_ERROR "rp6502_asset(<target> <address> <file>)")
        endif()
        list(GET args 1 in_file)
    endif()
    get_filename_component(src_file "${in_file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    file(RELATIVE_PATH rel_path "${CMAKE_SOURCE_DIR}" "${src_file}")
    if (IS_ABSOLUTE "${rel_path}" OR rel_path MATCHES "^\\.\\.")
        get_filename_component(rel_path "${src_file}" NAME)
    endif()
    string(MAKE_C_IDENTIFIER "${addr}" key)
    set(out_file "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}.rp6502/${key}/${rel_path}")
    get_filename_component(out_dir "${out_file}" DIRECTORY)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(create
        COMMAND "${Python3_EXECUTABLE}"
                "${RP6502_TOOLS_DIR}/rp6502.py"
                create
                -a "${addr}"
                -o "${out_file}"
                "${src_file}")
    if (sym)
        list(APPEND create --sym "$<TARGET_PROPERTY:${target},RP6502_SYM>")
    endif()
    set(create COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}" ${create})
    add_custom_command(
        OUTPUT "${out_file}"
        DEPENDS "${src_file}"
        ${create}
        VERBATIM
    )
    # The asset is never compiled as a source, whatever the file name.
    set_source_files_properties("${out_file}" PROPERTIES HEADER_FILE_ONLY TRUE)
    get_target_property(roms ${target} RP6502_ASSET_ROMS)
    list(LENGTH roms index)
    if (NOT roms)
        set(index 0)
    endif()
    set_target_properties(${target} PROPERTIES
        RP6502_ASSET_SRC_${index} "${src_file}"
        RP6502_ASSET_CMD_${index} "${create}")
    if (sym)
        set_property(TARGET ${target} PROPERTY RP6502_ASSET_SYM_${index} TRUE)
    endif()
    set_property(TARGET ${target} APPEND PROPERTY
        RP6502_ASSET_ROMS "${out_file}"
    )
    set_property(TARGET ${target} APPEND PROPERTY RP6502_ASSET_NAMES "${addr}")
endfunction()

#[=======================================================================[.rst:
.. command:: rp6502_executable

  Packages the linker output of a target as an RP6502 ROM.

  .. code-block:: cmake

    rp6502_executable(<target> [DATA <addr>] [NMI <addr>]
                      [RESET <addr>] [IRQ <addr>] [<rom>...])

  The linker output goes into ``<target>/`` in the current build folder,
  and building ``<target>`` also writes ``<target>.rp6502`` and
  ``<target>.sym`` there. The folder also holds ``rp6502-emu``, the emulator
  of the target. The ROM holds the linker output, the assets that
  :command:`rp6502_asset` and :command:`rp6502_byproduct` add, and each
  ``<rom>``, a path from the current source folder. The ``.sym`` file holds
  each integer ``#define`` of the C sources and each linker symbol, one
  ``NAME VALUE`` line each.
  Call :command:`rp6502_asset`, :command:`rp6502_byproduct`, and
  :command:`rp6502_align` for the target before this command.

  ``DATA <addr>``
    Where the linker output loads. Without ``DATA``, the ROM leaves the
    linker output out.

  ``NMI <addr>``, ``RESET <addr>``, ``IRQ <addr>``
    The vectors at $FFFA, $FFFC and $FFFE. ``RESET`` is required.

  An ``<addr>`` of ``file`` reads a 16-bit address from the start of the
  linker output, in the order of the keywords above. An ``<addr>`` of
  ``default`` is 0x200 for cc65 and ``file`` for llvm-mos.
#]=======================================================================]
function(rp6502_executable target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "DATA;RESET;IRQ;NMI" "")
    if (arg_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR
            "rp6502_executable(${target} ...): ${arg_KEYWORDS_MISSING_VALUES} needs an address.")
    endif()
    # An address of 0 is valid, so a keyword counts as given when it is
    # defined, whatever its value.
    foreach(key DATA RESET IRQ NMI)
        string(TOLOWER "${key}_addr" var)
        if (DEFINED arg_${key})
            set(${var} "${arg_${key}}")
        else()
            set(${var} "none")
        endif()
    endforeach()
    set(extra_roms ${arg_UNPARSED_ARGUMENTS})
    _rp6502_default_address(default_addr)
    foreach(V data_addr nmi_addr reset_addr irq_addr)
        if (${V} STREQUAL "default")
            set(${V} "${default_addr}")
        endif()
    endforeach()
    set(all_extra_roms)
    foreach(rom IN LISTS extra_roms)
        if (IS_ABSOLUTE "${rom}")
            list(APPEND all_extra_roms "${rom}")
        else()
            list(APPEND all_extra_roms "${CMAKE_CURRENT_SOURCE_DIR}/${rom}")
        endif()
    endforeach()
    # An asset made from a byproduct of the link, or in RAM() or XRAM(), is
    # made after the link, because its address can be in the .sym file.
    get_target_property(asset_roms ${target} RP6502_ASSET_ROMS)
    get_target_property(byproducts ${target} RP6502_BYPRODUCTS)
    set(pre_link)
    set(post_link)
    set(post_link_roms)
    set(sym_sources)
    if (asset_roms)
        list(APPEND all_extra_roms ${asset_roms})
        set(index 0)
        foreach(rom IN LISTS asset_roms)
            get_target_property(src ${target} RP6502_ASSET_SRC_${index})
            get_target_property(sym ${target} RP6502_ASSET_SYM_${index})
            get_target_property(cmd ${target} RP6502_ASSET_CMD_${index})
            if (byproducts AND src IN_LIST byproducts)
                list(APPEND post_link ${cmd})
                list(APPEND post_link_roms "${rom}")
            elseif (sym)
                list(APPEND post_link ${cmd})
                list(APPEND post_link_roms "${rom}")
                list(APPEND sym_sources "${src}")
            else()
                list(APPEND pre_link "${rom}")
            endif()
            math(EXPR index "${index} + 1")
        endforeach()
    endif()
    # The merge command of rp6502.py
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    _rp6502_target_folder(${target} folder)
    set(rom_file "${folder}/${target}.rp6502")
    set(tool_command "${Python3_EXECUTABLE}"
        "${RP6502_TOOLS_DIR}/rp6502.py"
        create
    )
    set(executable_inputs)
    if (NOT data_addr STREQUAL "none")
        list(APPEND tool_command -a "${data_addr}")
        list(APPEND executable_inputs "$<TARGET_FILE:${target}>")
    endif ()
    if (NOT nmi_addr STREQUAL "none")
        list(APPEND tool_command -n "${nmi_addr}")
    endif ()
    if (NOT reset_addr STREQUAL "none")
        list(APPEND tool_command -r "${reset_addr}")
    else ()
        message (FATAL_ERROR "rp6502_executable RESET address missing")
    endif ()
    if (NOT irq_addr STREQUAL "none")
        list(APPEND tool_command -i "${irq_addr}")
    endif ()
    list(APPEND tool_command
        -o "${rom_file}"
        ${executable_inputs}
        -- ${all_extra_roms}
    )
    # As sources, the assets are built before the link, and a change to one
    # links the target again. The ROM is a post-build step, so any build of
    # the target writes it.
    if (pre_link)
        target_sources(${target} PRIVATE ${pre_link})
    endif()
    # The source file of an asset with a symbol address is a source of the
    # target too, but is never compiled.
    if (sym_sources)
        set_source_files_properties(${sym_sources} PROPERTIES HEADER_FILE_ONLY TRUE)
        target_sources(${target} PRIVATE ${sym_sources})
    endif()
    if (post_link_roms)
        list(REMOVE_ITEM all_extra_roms ${post_link_roms})
    endif()
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        ${all_extra_roms} ${sym_sources})
    # The .sym file is written first, because rp6502.py reads some asset
    # addresses from it.
    set(sym_file "${folder}/${target}.sym")
    if (CMAKE_C_COMPILER_ID STREQUAL "cc65")
        set(linked "$<TARGET_FILE:${target}>.dbg")
    else()
        set(linked "$<TARGET_FILE:${target}>.elf")
    endif()
    get_property(aligns TARGET ${target} PROPERTY RP6502_ALIGN)
    set(align_args)
    foreach(regex IN LISTS aligns)
        list(APPEND align_args --align "${regex}")
    endforeach()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${RP6502_TOOLS_DIR}/rp6502.py"
                sym -o "${sym_file}" "${linked}"
                --commands "${CMAKE_BINARY_DIR}/compile_commands.json"
                --objects "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}.dir"
                --work "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}.sym"
                ${align_args}
        ${post_link}
        COMMAND ${CMAKE_COMMAND} -E rm -f "${rom_file}"
        COMMAND ${tool_command}
        BYPRODUCTS "${rom_file}" "${sym_file}" ${post_link_roms}
        VERBATIM
    )
    # rp6502.py sym reads the compile commands of the target.
    set_target_properties(${target} PROPERTIES
        EXPORT_COMPILE_COMMANDS ON
        RP6502_EXECUTABLE_CALLED TRUE
        RP6502_ROM "${rom_file}"
        RP6502_SYM "${sym_file}")
endfunction()

#[=======================================================================[.rst:
.. command:: rp6502_basic

  Packages BASIC programs as an RP6502 ROM.

  .. code-block:: cmake

    rp6502_basic(<target> [<autorun>])

  Building the executable target ``<target>`` writes ``<target>.rp6502``
  in ``<target>/`` in the current build folder. The ROM holds BASIC and the
  assets of ``<target>``. The programs are assets that
  :command:`rp6502_asset` adds before this call, as in
  ``rp6502_asset(<target> game.bas src/game.bas)``. One program starts
  another with ``RUN "ROM:<file>"``. The project lists BASIC as a
  language, as in ``project(<name> BASIC)`` or
  ``project(<name> C ASM BASIC)``, and a project with no compiler sets
  ``RP6502_BASIC`` in the configure preset. ``RP6502_BASIC_VERSION`` and
  ``RP6502_BASIC`` select BASIC, as `Tool Versions`_ tells.

  ``<autorun>``
    The asset that BASIC runs at start, through an added ``autorun.bas``
    asset. Without an ``autorun.bas`` asset, BASIC starts at the prompt.
#]=======================================================================]
function(rp6502_basic target)
    set(caller "rp6502_basic(${target})")
    if (ARGC GREATER 2 OR NOT TARGET ${target})
        message(FATAL_ERROR
            "rp6502_basic(<target> [<autorun>]), after add_executable(<target>) "
            "and its rp6502_asset() calls")
    endif()
    if (NOT CMAKE_BASIC_COMPILER_LOADED)
        message(FATAL_ERROR
            "${caller}: BASIC is not a language of the project. List it in "
            "project(), as in project(<name> BASIC) or project(<name> C ASM BASIC).")
    endif()
    if (ARGC EQUAL 2)
        set(dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}.basic")
        # The ROM: drive ignores case, and BASIC is written in capitals.
        string(TOUPPER "${ARGV1}" autorun)
        get_target_property(names ${target} RP6502_ASSET_NAMES)
        string(TOUPPER "${names}" names)
        if (NOT autorun IN_LIST names)
            message(FATAL_ERROR
                "${caller}: no rp6502_asset(${target} ${ARGV1} ...) comes before it.")
        endif()
        # file(CONFIGURE) keeps the ROM from rebuilding at every configure.
        file(CONFIGURE OUTPUT "${dir}/autorun.bas" CONTENT "10 RUN \"ROM:${autorun}\"\n")
        rp6502_asset(${target} autorun.bas "${dir}/autorun.bas")
    endif()
    get_target_property(assets ${target} RP6502_ASSET_ROMS)
    if (NOT assets)
        set(assets)
    endif()
    # As sources, the assets are built before the link that merges them.
    target_sources(${target} PRIVATE ${assets})
    _rp6502_target_folder(${target} folder)
    set(rom "${folder}/${target}.rp6502")
    set_target_properties(${target} PROPERTIES LINKER_LANGUAGE BASIC SUFFIX "")
    # The link writes the ROM, and Ninja requires a rule that declares it.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E true
        BYPRODUCTS "${rom}"
        VERBATIM)
    # The end of configure adds BASIC, because its version can be set later.
    set_target_properties(${target} PROPERTIES
        RP6502_EXECUTABLE_CALLED TRUE
        RP6502_BASIC_TARGET TRUE
        RP6502_ROM "${rom}")
endfunction()

#[=======================================================================[.rst:
.. command:: rp6502_web

  Packages a ROM as a web page that runs the ROM in the emulator.

  .. code-block:: cmake

    rp6502_web(<target> [OUTPUT <name>.zip]
               [PAGE <file> | <folder>] [CONFIG <text>])

  ``<target>`` must already have a ROM from :command:`rp6502_executable`
  or :command:`rp6502_basic`. Building ``<target>`` also writes
  ``web/<name>.zip`` in the build folder, with the same files unpacked in
  ``web/<name>/``. At the root are the ROM, the page as ``index.html``,
  and ``rp6502.js`` and ``rp6502.wasm`` from the web player.
  ``RP6502_WEB_VERSION`` and ``RP6502_WEB`` select the web player. See
  `Tool Versions`_.

  ``OUTPUT <name>.zip``
    The name of the zip, by default ``<target>.zip``.

  ``PAGE <file> | <folder>``
    The page file, or a folder copied with all subfolders, where the root
    ``index.html`` is the page. The default is the ``index.html`` of the
    web player. The page must load the emulator with
    ``<script src="rp6502.js">`` and then call
    ``rp6502(container, rom, options)``. A script inserted right after the
    ``rp6502.js`` tag replaces the ``rom`` of every ``rp6502()`` call with
    the ROM of ``<target>``, and merges ``CONFIG`` into the ``options`` of
    every call.

  ``CONFIG <text>``
    JavaScript, the keys and values of an object, which replace or add
    keys of the ``options`` of every ``rp6502()`` call in the page.
    ``github``, for the links under a footer, is the GitHub repository of
    the git remote ``origin`` unless ``<text>`` names another. With the
    default page, ``title`` is blank unless ``<text>`` sets it.
#]=======================================================================]
function(rp6502_web target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "OUTPUT;PAGE;CONFIG" "")
    set(caller "rp6502_web(${target})")
    if (arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "${caller}: ${arg_UNPARSED_ARGUMENTS} follows no keyword. rp6502_web(<target> "
            "[OUTPUT <name>.zip] [PAGE <file> | <folder>] [CONFIG <text>])")
    endif()
    if (TARGET ${target})
        get_target_property(rom_file ${target} RP6502_ROM)
    endif()
    if (NOT rom_file)
        message(FATAL_ERROR
            "${caller}: ${target} makes no ROM. Call rp6502_executable(${target} ...) "
            "or rp6502_basic(${target} ...) before rp6502_web(${target} ...).")
    endif()
    set(zip "${target}.zip")
    if (arg_OUTPUT)
        set(zip "${arg_OUTPUT}")
    endif()
    if (NOT zip MATCHES "^[A-Za-z0-9_-][A-Za-z0-9._-]*\\.zip$")
        message(FATAL_ERROR "${caller}: OUTPUT ${zip} is not a file name ending in .zip.")
    endif()
    string(REGEX REPLACE "\\.zip$" "" name "${zip}")
    get_property(zips GLOBAL PROPERTY RP6502_WEB_ZIPS)
    if (zip IN_LIST zips)
        message(FATAL_ERROR "${caller}: two rp6502_web() calls make ${zip}.")
    endif()
    set_property(GLOBAL APPEND PROPERTY RP6502_WEB_ZIPS "${zip}")
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${name}.web")

    # An empty page is the index.html of the web player.
    set(page)
    set(folder)
    if (arg_PAGE)
        get_filename_component(path "${arg_PAGE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if (IS_DIRECTORY "${path}")
            set(folder "${path}")
            if (EXISTS "${path}/index.html")
                set(page "${path}/index.html")
            endif()
        elseif (EXISTS "${path}")
            set(page "${path}")
        else()
            message(FATAL_ERROR "${caller}: PAGE ${arg_PAGE} is not a file or a folder.")
        endif()
    endif()
    set(folder_files)
    set(folder_entries)
    if (folder)
        file(GLOB_RECURSE found LIST_DIRECTORIES false CONFIGURE_DEPENDS "${folder}/*")
        foreach(file IN LISTS found)
            file(RELATIVE_PATH relative "${folder}" "${file}")
            if (relative STREQUAL "${target}.rp6502" OR relative STREQUAL "rp6502.js"
                    OR relative STREQUAL "rp6502.wasm")
                message(FATAL_ERROR "${caller}: ${arg_PAGE}/${relative} has the name of a file rp6502_web() makes.")
            endif()
            list(APPEND folder_files "${file}")
        endforeach()
        file(GLOB folder_entries LIST_DIRECTORIES true RELATIVE "${folder}" "${folder}/*")
        list(REMOVE_ITEM folder_entries index.html)
    endif()
    if (page)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${page}")
    endif()

    set(merge)
    if (NOT page)
        string(APPEND merge ", {title: ''}")
    endif()
    execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}" remote get-url origin
        OUTPUT_VARIABLE origin OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (origin MATCHES "github\\.com[:/]([^/]+/[^/]+)$")
        string(REGEX REPLACE "\\.git$" "" repo "${CMAKE_MATCH_1}")
        string(APPEND merge ", {github: '${repo}'}")
    endif()
    # web.py takes the last value of a key in the text, so CONFIG is written
    # after the other options, in the order of the merge.
    set(config)
    set(options "options")
    if (DEFINED arg_CONFIG)
        # Migration: run, the old name of autoplay, is accepted until the
        # projects that use it change to autoplay.
        set(key "(^|[^A-Za-z0-9_$])")
        set(run "${key}run[ \t\r\n]*:[ \t\r\n]*['\"]([A-Za-z]*)['\"]")
        if (arg_CONFIG MATCHES "${run}")
            set(old "${CMAKE_MATCH_2}")
            set(value "${old}")
            if (arg_CONFIG MATCHES "${key}autoplay[ \t\r\n]*:")
                message(FATAL_ERROR "${caller}: CONFIG sets both run and autoplay.")
            elseif (value STREQUAL "always")
                set(value on)
            elseif (value STREQUAL "onaudio")
                set(value auto)
            elseif (value STREQUAL "onclick")
                set(value off)
            else()
                message(FATAL_ERROR "${caller}: run in CONFIG must be always, onaudio or onclick.")
            endif()
            message(WARNING "${caller}: in CONFIG, change run: '${old}' to autoplay: '${value}'.")
            string(REGEX REPLACE "${run}" "\\1autoplay: '${value}'" arg_CONFIG "${arg_CONFIG}")
        endif()
        set(config "    const config = {\n${arg_CONFIG}\n};\n")
        set(options "Object.assign(options, config)")
    endif()
    # The script follows the rp6502.js tag, so it replaces rp6502() before any
    # call in the page.
    string(CONCAT script
        "\n<script>\n"
        "// rp6502_web()\n"
        "rp6502 = ((call) => (container, page_rom, options) => {\n"
        "    const rom = '${target}.rp6502';\n"
        "    options = Object.assign({}, options${merge});\n"
        "${config}"
        "    return call(container, rom, ${options});\n"
        "})(rp6502);\n"
        "</script>")
    # The end of configure writes the page, because the version of the web
    # player can be set later.
    set_property(GLOBAL PROPERTY RP6502_WEB_TARGET_${name} "${target}")
    set_property(GLOBAL PROPERTY RP6502_WEB_DIR_${name} "${dir}")
    set_property(GLOBAL PROPERTY RP6502_WEB_PAGE_${name} "${page}")
    set_property(GLOBAL PROPERTY RP6502_WEB_SCRIPT_${name} "${script}")
    set_property(GLOBAL PROPERTY RP6502_WEB_FILES_${name} "${folder_files}")
    set(out "${CMAKE_BINARY_DIR}/web")

    set(stage "${out}/${name}")
    set(copy_folder)
    if (folder)
        set(copy_folder COMMAND "${CMAKE_COMMAND}" -E copy_directory "${folder}" "${stage}")
    endif()
    # The zip is a post-build step of the ROM target, and a change to the
    # page, the folder or the web player links the target again.
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        "${dir}/index.html" "${dir}/sources.txt" ${folder_files})
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${stage}" "${out}/${zip}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${stage}"
        ${copy_folder}
        COMMAND "${CMAKE_COMMAND}" -E copy
            "${dir}/emulator/rp6502.js" "${dir}/emulator/rp6502.wasm" "${dir}/index.html"
            "${stage}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${rom_file}" "${stage}/${target}.rp6502"
        COMMAND "${CMAKE_COMMAND}" -E chdir "${stage}"
            "${CMAKE_COMMAND}" -E tar cf "${out}/${zip}" --format=zip --
            index.html rp6502.js rp6502.wasm ${target}.rp6502 ${folder_entries}
        BYPRODUCTS "${out}/${zip}"
        COMMENT "Packaging web/${zip}"
        VERBATIM
    )
endfunction()

#[=======================================================================[.rst:
.. command:: rp6502_test

  Adds a test that runs in the emulator, in the form of ``add_test()``.

  .. code-block:: cmake

    rp6502_test(NAME <name> COMMAND <file> [<arg>...])

  The test ``<name>`` runs ``<file>``, a path from the current source
  folder: a ``.py`` file in Python, and any other file as an emulator
  script. Each name must be unique in the project. The test runs in
  ``test/<name>/`` in the build folder. That folder holds ``rp6502-emu``,
  the emulator of the test, and ``save/``, the folder behind ``SAVE:``. A
  script runs as ``rp6502-emu --save-dir <save> --script <file> <arg>...``.
  A Python test runs as ``python <file> --save-dir <save> <arg>...``, with
  ``tools/`` in ``PYTHONPATH`` and the emulator in the environment
  variable ``RP6502_EMU``. Each ``<arg>`` is an emulator option or a ROM,
  such as ``$<TARGET_PROPERTY:hello,RP6502_ROM>``, and the words after
  ``--`` are the arguments of the ROM.

  The tests also add two setup tests, which ctest runs before the other
  tests: ``rp6502.build`` builds the project, and ``rp6502.save`` empties
  each ``save/`` folder.
#]=======================================================================]
function(rp6502_test)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME" "COMMAND")
    if (NOT DEFINED arg_NAME OR NOT DEFINED arg_COMMAND OR arg_UNPARSED_ARGUMENTS
            OR arg_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "rp6502_test(NAME <name> COMMAND <file> [<arg>...])")
    endif()
    set(name "${arg_NAME}")
    set(caller "rp6502_test(NAME ${name})")
    set(args "${arg_COMMAND}")
    list(POP_FRONT args file)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_filename_component(path "${file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if (NOT EXISTS "${path}")
        message(FATAL_ERROR "${caller}: ${file} is not a file.")
    endif()
    # CMake refuses a repeated test name only inside one folder.
    get_property(names GLOBAL PROPERTY RP6502_TESTS)
    if (name IN_LIST names)
        message(FATAL_ERROR "${caller}: the project already has a test named ${name}.")
    endif()
    set_property(GLOBAL APPEND PROPERTY RP6502_TESTS "${name}")
    # The end of configure links the emulator, from the settings of this
    # folder.
    set_property(GLOBAL PROPERTY RP6502_TEST_DIR_${name} "${CMAKE_CURRENT_SOURCE_DIR}")
    # ctest does not make the working folder.
    set(dir "${CMAKE_BINARY_DIR}/test/${name}")
    file(MAKE_DIRECTORY "${dir}")
    set(emu "${dir}/${RP6502_EMU_NAME}")
    if (path MATCHES "\\.py$")
        add_test(NAME ${name}
            COMMAND "${Python3_EXECUTABLE}" "${path}" --save-dir "${dir}/save" ${args}
            WORKING_DIRECTORY "${dir}")
        set_tests_properties(${name} PROPERTIES ENVIRONMENT
            "PYTHONPATH=${RP6502_TOOLS_DIR};PYTHONDONTWRITEBYTECODE=1;RP6502_EMU=${emu}")
    else()
        add_test(NAME ${name}
            COMMAND "${emu}" --save-dir "${dir}/save" --script "${path}" ${args}
            WORKING_DIRECTORY "${dir}")
    endif()
    set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED rp6502.build)
endfunction()

#[=======================================================================[.rst:
.. command:: rp6502_byproduct

  Adds ``<file>``, which the link of ``<target>`` writes, to the ROM of
  ``<target>``.

  .. code-block:: cmake

    rp6502_byproduct(<target> <address> <file>)
    rp6502_byproduct(<target> RAM(<x>) <file>)
    rp6502_byproduct(<target> XRAM(<x>) <file>)

  ``<address>`` is as in :command:`rp6502_asset`. A relative ``<file>`` is a
  path from the folder of the linker output, ``<target>/`` in the current
  build folder. The build packages the file after the link, so a
  ``POST_BUILD`` command of the target can write it. Add that command before
  :command:`rp6502_executable`. A ``POST_BUILD`` command runs only when the
  target links, so add its input files to the ``LINK_DEPENDS`` property of
  the target. Call this before :command:`rp6502_executable`.
#]=======================================================================]
function(rp6502_byproduct target)
    set(args ${ARGN})
    list(LENGTH args argc)
    if (NOT argc EQUAL 2 AND NOT argc EQUAL 5)
        message(FATAL_ERROR "rp6502_byproduct(<target> <address> <file>)")
    endif()
    list(POP_BACK args file)
    get_filename_component(file "${file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}/${target}")
    set_property(TARGET ${target} APPEND PROPERTY RP6502_BYPRODUCTS "${file}")
    rp6502_asset(${target} ${args} "${file}")
endfunction()

# Internal helpers of the commands above

# cc65 links a flat image at a fixed address;
# llvm-mos writes the address into the start of the output file.
function(_rp6502_default_address var)
    if(CMAKE_C_COMPILER_ID STREQUAL "cc65")
        set(${var} 0x200 PARENT_SCOPE)
    else()
        set(${var} file PARENT_SCOPE)
    endif()
endfunction()

# Puts the output of <target> in <current build folder>/<target>/, and sets
# <out_var> to that folder. A file at that path, the linker output of older
# tools, is deleted.
function(_rp6502_target_folder target out_var)
    set(folder "${CMAKE_CURRENT_BINARY_DIR}/${target}")
    if(EXISTS "${folder}" AND NOT IS_DIRECTORY "${folder}")
        file(REMOVE "${folder}")
    endif()
    file(MAKE_DIRECTORY "${folder}")
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${folder}")
    set_property(GLOBAL APPEND PROPERTY RP6502_TARGETS ${target})
    set(${out_var} "${folder}" PARENT_SCOPE)
endfunction()

# Sets <out_var> to the setting <name> of <item>, a TARGET or a TEST in the
# source folder <dir>: the cache, else the property, else the variable. With
# FILE, a CMake boolean constant counts as no value.
function(_rp6502_setting name want kind item dir out_var)
    set(cache)
    if(DEFINED CACHE{${name}})
        set(cache "$CACHE{${name}}")
    endif()
    if(kind STREQUAL "TARGET")
        get_target_property(property ${item} ${name})
    else()
        get_test_property(${item} ${name} DIRECTORY "${dir}" property)
    endif()
    get_directory_property(variable DIRECTORY "${dir}" DEFINITION ${name})
    foreach(level cache property variable)
        set(value "${${level}}")
        if(value STREQUAL "" OR value MATCHES "NOTFOUND$")
            continue()
        endif()
        string(TOUPPER "${value}" upper)
        if(want STREQUAL "FILE" AND upper MATCHES "^(1|0|ON|OFF|YES|NO|TRUE|FALSE|Y|N|IGNORE)$")
            continue()
        endif()
        set(${out_var} "${value}" PARENT_SCOPE)
        return()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

# Sets <file_var> to the emulator, the zip of the web player or the
# basic.rp6502 of <tool> (EMU, WEB or BASIC) for <item>, a TARGET or a TEST
# in the source folder <dir>, and <source_var> to "file <path>" or
# "release <repo> v<version>". The file of EMU is empty when the host has no
# emulator.
function(_rp6502_resolve tool kind item dir file_var source_var)
    _rp6502_setting(RP6502_${tool} FILE ${kind} ${item} "${dir}" file)
    if(NOT file STREQUAL "")
        get_filename_component(file "${file}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
        if(NOT EXISTS "${file}" OR IS_DIRECTORY "${file}")
            message(FATAL_ERROR "RP6502_${tool} ${file} is not a file.")
        endif()
        # A rebuilt file replaces the links to it at the next configure.
        set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
        set(${file_var} "${file}" PARENT_SCOPE)
        set(${source_var} "file ${file}" PARENT_SCOPE)
        return()
    endif()
    _rp6502_setting(RP6502_${tool}_VERSION VERSION ${kind} ${item} "${dir}" version)
    if(NOT version STREQUAL "" AND NOT version MATCHES "^[0-9][0-9A-Za-z.+_-]*$")
        message(FATAL_ERROR "RP6502_${tool}_VERSION ${version}: a version starts with a digit, such as 0.40.")
    endif()
    # Every target and test with the same version gets the same file.
    set(key "RP6502_RESOLVED_${tool}_${version}")
    get_property(done GLOBAL PROPERTY ${key} SET)
    if(NOT done)
        _rp6502_release(${tool} "${version}" file version repo)
        if(tool STREQUAL "BASIC")
            _rp6502_unpack("${file}" folder)
            set(file "${folder}/basic.rp6502")
            if(NOT EXISTS "${file}")
                message(FATAL_ERROR "msbasic-${version}.zip has no basic.rp6502.")
            endif()
        elseif(tool STREQUAL "EMU" AND NOT file STREQUAL "")
            _rp6502_unpack("${file}" folder)
            set(file "${folder}/${RP6502_HOST_MEMBER}")
            if(NOT EXISTS "${file}")
                message(FATAL_ERROR "${folder} has no ${RP6502_HOST_MEMBER}.")
            endif()
            if(NOT CMAKE_HOST_WIN32)
                file(CHMOD "${file}" PERMISSIONS
                    OWNER_READ OWNER_WRITE OWNER_EXECUTE
                    GROUP_READ GROUP_EXECUTE
                    WORLD_READ WORLD_EXECUTE)
            endif()
        endif()
        set_property(GLOBAL PROPERTY ${key} "${file}" "release ${repo} v${version}")
    endif()
    get_property(done GLOBAL PROPERTY ${key})
    list(GET done 0 file)
    list(GET done 1 source)
    set(${file_var} "${file}" PARENT_SCOPE)
    set(${source_var} "${source}" PARENT_SCOPE)
endfunction()

# Sets <file_var> to the file in tools/ of release <version> of <tool>, and
# <version_var> and <repo_var> to the version and repository. An empty
# <version> is the newest release in tools/, else the latest release.
function(_rp6502_release tool version file_var version_var repo_var)
    if(tool STREQUAL "BASIC")
        set(repo "${RP6502_BASIC_REPO}")
        set(name "msbasic-@.zip")
        set(versions "^msbasic-(.+)\\.zip$")
    else()
        set(repo "${RP6502_TOOLS_REPO}")
        set(versions "^rp6502-(.+)-web\\.zip$")
        if(tool STREQUAL "WEB")
            set(name "rp6502-@-web.zip")
        else()
            set(name "rp6502-@-${RP6502_HOST_SUFFIX}")
        endif()
    endif()
    set(required TRUE)
    if(tool STREQUAL "EMU")
        set(required FALSE)
        if(NOT RP6502_HOST_SUFFIX)
            message(NOTICE "No emulator: no release has one for this host.")
            set(${file_var} "" PARENT_SCOPE)
            set(${version_var} "" PARENT_SCOPE)
            set(${repo_var} "${repo}" PARENT_SCOPE)
            return()
        endif()
    endif()
    if(version STREQUAL "")
        string(REPLACE "." "\\." pattern "${name}")
        string(REPLACE "@" "(.+)" pattern "^${pattern}(\\.unsupported)?$")
        _rp6502_newest("${pattern}" version)
    endif()
    if(version STREQUAL "")
        _rp6502_latest("${repo}" "${versions}" ${required} version)
    endif()
    set(file "")
    if(NOT version STREQUAL "")
        string(REPLACE "@" "${version}" name "${name}")
        _rp6502_fetch_release("${repo}" "${version}" "${name}" ${required} file)
    endif()
    set(${file_var} "${file}" PARENT_SCOPE)
    set(${version_var} "${version}" PARENT_SCOPE)
    set(${repo_var} "${repo}" PARENT_SCOPE)
endfunction()

# Sets <out_var> to the newest version among the names in tools/ that match
# <regex>, where the first group is the version, or to "" for none.
function(_rp6502_newest regex out_var)
    file(GLOB names RELATIVE "${RP6502_TOOLS_DIR}" "${RP6502_TOOLS_DIR}/*")
    set(newest "")
    foreach(name IN LISTS names)
        if(name MATCHES "${regex}")
            if(newest STREQUAL "" OR CMAKE_MATCH_1 VERSION_GREATER newest)
                set(newest "${CMAKE_MATCH_1}")
            endif()
        endif()
    endforeach()
    set(${out_var} "${newest}" PARENT_SCOPE)
endfunction()

# Sets <out_var> to the version of the latest release of <repo>, read from
# the name in its SHA256SUMS that matches <regex>. A failure stops configure
# when <required> is true, and otherwise sets <out_var> to "".
function(_rp6502_latest repo regex required out_var)
    get_property(version GLOBAL PROPERTY RP6502_LATEST_${repo})
    if("${version}" STREQUAL "")
        get_filename_component(stem "${repo}" NAME)
        set(sums "${RP6502_TOOLS_DIR}/${stem}-latest.SHA256SUMS.tmp")
        set(url "https://github.com/${repo}/releases/latest/download/SHA256SUMS")
        _rp6502_download("${url}" "${sums}" "" result text)
        set(version "")
        if(result STREQUAL "ok")
            _rp6502_read_sums("${sums}" assets)
            file(REMOVE "${sums}")
            foreach(asset IN LISTS assets)
                string(REGEX REPLACE "=[0-9a-fA-F]+$" "" asset_name "${asset}")
                if(asset_name MATCHES "${regex}")
                    set(version "${CMAKE_MATCH_1}")
                endif()
            endforeach()
            if(version STREQUAL "")
                set(text "${url} lists no file that matches ${regex}")
            endif()
        endif()
        if(version STREQUAL "")
            if(required)
                message(FATAL_ERROR "Cannot find the latest release of ${repo}.\n${text}")
            endif()
            message(NOTICE "No emulator: cannot find the latest release of ${repo}.\n${text}")
            set(${out_var} "" PARENT_SCOPE)
            return()
        endif()
        set_property(GLOBAL PROPERTY RP6502_LATEST_${repo} "${version}")
    endif()
    set(${out_var} "${version}" PARENT_SCOPE)
endfunction()

# Sets <out_var> to tools/<name> of release v<version> of <repo>, fetched
# when tools/ lacks it. A release without the file stops configure for a
# <required> file, and otherwise writes tools/<name>.unsupported.
function(_rp6502_fetch_release repo version name required out_var)
    set(file "${RP6502_TOOLS_DIR}/${name}")
    set(record "${file}.unsupported")
    set(${out_var} "" PARENT_SCOPE)
    if(EXISTS "${file}")
        set(${out_var} "${file}" PARENT_SCOPE)
        return()
    endif()
    if(EXISTS "${record}")
        file(READ "${record}" text)
        message(NOTICE "No emulator: ${text}")
        return()
    endif()
    set(base "https://github.com/${repo}/releases/download/v${version}")
    get_filename_component(stem "${repo}" NAME)
    set(sums "${RP6502_TOOLS_DIR}/${stem}-${version}.SHA256SUMS.tmp")
    _rp6502_download("${base}/SHA256SUMS" "${sums}" "" result text)
    if(result STREQUAL "missing")
        message(FATAL_ERROR "${repo} has no release v${version}.")
    endif()
    set(hash "")
    if(result STREQUAL "ok")
        _rp6502_read_sums("${sums}" assets)
        file(REMOVE "${sums}")
        foreach(asset IN LISTS assets)
            if(asset MATCHES "^(.+)=([0-9a-fA-F]+)$" AND CMAKE_MATCH_1 STREQUAL name)
                set(hash "${CMAKE_MATCH_2}")
            endif()
        endforeach()
        if(hash STREQUAL "")
            set(text "release v${version} of ${repo} has no ${name}.")
            if(required)
                message(FATAL_ERROR "The ${text}")
            endif()
            file(WRITE "${record}" "${text}\n")
            message(NOTICE "No emulator: ${text}")
            return()
        endif()
        message(STATUS "Fetching tools/${name}")
        _rp6502_download("${base}/${name}" "${file}.tmp" "${hash}" result text)
    endif()
    if(NOT result STREQUAL "ok")
        if(required)
            message(FATAL_ERROR "Cannot fetch tools/${name}.\n${text}")
        endif()
        message(NOTICE "No emulator: cannot fetch tools/${name}.\n${text}")
        return()
    endif()
    file(RENAME "${file}.tmp" "${file}")
    set(${out_var} "${file}" PARENT_SCOPE)
endfunction()

# Downloads <url> to <file>, checked against <sha256> when it is not empty.
# Sets <out_var> to ok, missing for HTTP 404, or failed, and <text_var> to
# the reason.
function(_rp6502_download url file sha256 out_var text_var)
    file(DOWNLOAD "${url}" "${file}"
        STATUS status
        LOG log
        TLS_VERIFY ON
        INACTIVITY_TIMEOUT 30
    )
    list(GET status 0 code)
    list(GET status 1 text)
    # The last status line is the answer after any redirects.
    string(REGEX MATCHALL "HTTP/[0-9.]+ [0-9][0-9][0-9]" answers "${log}")
    set(answer "")
    if(answers)
        list(GET answers -1 answer)
        string(REGEX REPLACE "^HTTP/[0-9.]+ " "" answer "${answer}")
    endif()
    set(result ok)
    if(NOT code EQUAL 0)
        file(REMOVE "${file}")
        set(result failed)
        # curl reports an error answer as 22.
        if(code EQUAL 22)
            set(text "HTTP ${answer}")
            if(answer STREQUAL "404")
                set(result missing)
            endif()
        endif()
    elseif(NOT sha256 STREQUAL "")
        file(SHA256 "${file}" got)
        string(TOLOWER "${sha256}" expected)
        if(NOT got STREQUAL expected)
            file(REMOVE "${file}")
            set(result failed)
            set(text "wrong contents, SHA256 ${got} where ${expected} was expected")
        endif()
    endif()
    set(${out_var} ${result} PARENT_SCOPE)
    set(${text_var} "${url}: ${text}" PARENT_SCOPE)
endfunction()

# Unpacks <archive> once into build/tools/<name without .zip or .tar.gz>/
# in the project, and sets <out_var> to that folder.
function(_rp6502_unpack archive out_var)
    get_filename_component(name "${archive}" NAME)
    string(REGEX REPLACE "\\.(zip|tar\\.gz)$" "" stem "${name}")
    set(folder "${RP6502_PROJECT_DIR}/build/tools/${stem}")
    if(NOT IS_DIRECTORY "${folder}")
        # The rename comes last, so an interrupted unpack leaves no folder.
        set(tmp "${folder}.tmp")
        file(REMOVE_RECURSE "${tmp}")
        file(MAKE_DIRECTORY "${tmp}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E tar xf "${archive}"
            WORKING_DIRECTORY "${tmp}"
            RESULT_VARIABLE result
            ERROR_VARIABLE error
        )
        if(NOT result STREQUAL "0")
            file(REMOVE_RECURSE "${tmp}")
            message(FATAL_ERROR "Cannot unpack ${archive}\n${error}")
        endif()
        file(RENAME "${tmp}" "${folder}")
    endif()
    set(${out_var} "${folder}" PARENT_SCOPE)
endfunction()

# Makes <link> a hard link to <file>, or a copy where the file system has no
# links, and keeps a link with the size and time of <file>. An empty <file>
# removes <link>.
function(_rp6502_link file link)
    if(file STREQUAL "")
        file(REMOVE "${link}")
        return()
    endif()
    if(EXISTS "${link}")
        file(SIZE "${file}" size)
        file(SIZE "${link}" link_size)
        file(TIMESTAMP "${file}" time "%s" UTC)
        file(TIMESTAMP "${link}" link_time "%s" UTC)
        if(size EQUAL link_size AND time STREQUAL link_time)
            return()
        endif()
    endif()
    get_filename_component(folder "${link}" DIRECTORY)
    file(MAKE_DIRECTORY "${folder}")
    # The rename comes last, so a command line that holds <link> runs the
    # old file or the new file, and never a part of one.
    set(tmp "${link}.tmp")
    file(REMOVE_RECURSE "${tmp}" "${tmp}.d")
    file(CREATE_LINK "${file}" "${tmp}" RESULT result)
    if(NOT result STREQUAL "0")
        # file(COPY) keeps the time of the file, which the check above reads.
        file(COPY "${file}" DESTINATION "${tmp}.d")
        get_filename_component(name "${file}" NAME)
        file(RENAME "${tmp}.d/${name}" "${tmp}")
        file(REMOVE_RECURSE "${tmp}.d")
    endif()
    file(RENAME "${tmp}" "${link}" RESULT result)
    if(NOT result STREQUAL "0")
        file(REMOVE "${tmp}")
        message(NOTICE "Cannot replace ${link}. Close the emulator, then configure again.\n${result}")
    endif()
endfunction()

# Adds BASIC to the link of the BASIC target <target> in the source folder <dir>.
function(_rp6502_basic_finish target dir)
    _rp6502_resolve(BASIC TARGET ${target} "${dir}" basic source)
    get_target_property(binary ${target} BINARY_DIR)
    # The file changes only with BASIC, so a Makefile relinks for a new BASIC
    # that is older than the ROM.
    set(record "${binary}/CMakeFiles/${target}.basic/basic.txt")
    file(CONFIGURE OUTPUT "${record}" CONTENT "${basic}\n")
    get_target_property(assets ${target} RP6502_ASSET_ROMS)
    if(NOT assets)
        set(assets)
    endif()
    target_link_options(${target} PRIVATE "${basic}" ${assets})
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        "${basic}" "${record}" ${assets})
endfunction()

# Writes the page and the emulator files that the build of web/<zip> copies.
function(_rp6502_web_finish zip)
    string(REGEX REPLACE "\\.zip$" "" name "${zip}")
    get_property(target GLOBAL PROPERTY RP6502_WEB_TARGET_${name})
    get_property(dir GLOBAL PROPERTY RP6502_WEB_DIR_${name})
    get_property(page GLOBAL PROPERTY RP6502_WEB_PAGE_${name})
    get_property(script GLOBAL PROPERTY RP6502_WEB_SCRIPT_${name})
    get_property(folder_files GLOBAL PROPERTY RP6502_WEB_FILES_${name})
    set(caller "rp6502_web(${target})")
    get_target_property(src ${target} SOURCE_DIR)
    _rp6502_resolve(WEB TARGET ${target} "${src}" web_player source)

    file(REMOVE_RECURSE "${dir}/emulator")
    file(ARCHIVE_EXTRACT INPUT "${web_player}" DESTINATION "${dir}/emulator"
        PATTERNS rp6502.js rp6502.wasm index.html)
    foreach(part rp6502.js rp6502.wasm index.html)
        if(NOT EXISTS "${dir}/emulator/${part}")
            message(FATAL_ERROR "${caller}: ${web_player} has no ${part} at its root.")
        endif()
    endforeach()
    if(NOT page)
        set(page "${dir}/emulator/index.html")
    endif()

    file(READ "${page}" html)
    string(FIND "${html}" "<script src=\"rp6502.js\"" at)
    if(at LESS 0)
        message(FATAL_ERROR "${caller}: ${page} has no <script src=\"rp6502.js\">.")
    endif()
    string(SUBSTRING "${html}" ${at} -1 tail)
    string(FIND "${tail}" "</script>" end)
    if(end LESS 0)
        message(FATAL_ERROR "${caller}: ${page} has no </script> after <script src=\"rp6502.js\">.")
    endif()
    math(EXPR at "${at} + ${end} + 9")
    string(SUBSTRING "${html}" 0 ${at} head)
    string(SUBSTRING "${html}" ${at} -1 tail)
    set(html "${head}${script}${tail}")
    # index.html is rewritten only on a change, so the zip is not rebuilt.
    set(written)
    if(EXISTS "${dir}/index.html")
        file(READ "${dir}/index.html" written)
    endif()
    if(NOT written STREQUAL html)
        file(WRITE "${dir}/index.html" "${html}")
    endif()
    file(SHA256 "${web_player}" web_hash)
    string(REPLACE ";" "\n" listed "${folder_files}")
    file(CONFIGURE OUTPUT "${dir}/sources.txt"
        CONTENT "${web_player} ${web_hash}\n${listed}\n" @ONLY)
    # The publish workflow reads this file to screenshot the player with the
    # same emulator.
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/CMakeFiles/${name}.emulator"
        CONTENT "${source}\n" @ONLY)
endfunction()

# Gives each target and test its emulator, BASIC and web player at the end
# of configure, after every setting is made.
function(_rp6502_finish)
    get_property(targets GLOBAL PROPERTY RP6502_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(dir ${target} SOURCE_DIR)
        get_target_property(folder ${target} RUNTIME_OUTPUT_DIRECTORY)
        _rp6502_resolve(EMU TARGET ${target} "${dir}" emu source)
        _rp6502_link("${emu}" "${folder}/${RP6502_EMU_NAME}")
        get_target_property(basic ${target} RP6502_BASIC_TARGET)
        if(basic)
            _rp6502_basic_finish(${target} "${dir}")
        endif()
    endforeach()
    get_property(zips GLOBAL PROPERTY RP6502_WEB_ZIPS)
    foreach(zip IN LISTS zips)
        _rp6502_web_finish("${zip}")
    endforeach()
    get_property(tests GLOBAL PROPERTY RP6502_TESTS)
    set(saves)
    foreach(name IN LISTS tests)
        get_property(dir GLOBAL PROPERTY RP6502_TEST_DIR_${name})
        set(folder "${CMAKE_BINARY_DIR}/test/${name}")
        _rp6502_resolve(EMU TEST ${name} "${dir}" emu source)
        _rp6502_link("${emu}" "${folder}/${RP6502_EMU_NAME}")
        list(APPEND saves "${folder}/save")
    endforeach()
    if(tests)
        # ctest does not build the ROMs, and a test starts with an empty save/.
        add_test(NAME rp6502.build COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}")
        add_test(NAME rp6502.save COMMAND "${CMAKE_COMMAND}" -E rm -rf ${saves})
        set_tests_properties(rp6502.build rp6502.save PROPERTIES FIXTURES_SETUP rp6502.build)
    endif()
    # A new emulator, web player or BASIC in tools/ runs configure again at
    # the next build.
    file(GLOB releases CONFIGURE_DEPENDS
        "${RP6502_TOOLS_DIR}/rp6502-*" "${RP6502_TOOLS_DIR}/msbasic-*")
endfunction()

# Used by the commands and the tool update

# Reads sha256sum(1) output as a list of name=hash.
function(_rp6502_read_sums file out_var)
    file(STRINGS "${file}" lines)
    set(entries)
    foreach(line IN LISTS lines)
        if(line MATCHES "^([0-9a-fA-F]+)[ \t]+([^ \t/\\\\]+)$")
            list(APPEND entries "${CMAKE_MATCH_2}=${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set(${out_var} "${entries}" PARENT_SCOPE)
endfunction()

# Sets RP6502_HOST_SUFFIX to the end of the name of the file that holds
# the emulator for this host, RP6502_HOST_MEMBER to the emulator in that
# file, and RP6502_EMU_NAME to the name of the links to the emulator.
function(_rp6502_host)
    set(suffix)
    set(member "rp6502-emu")
    set(exe "rp6502-emu")
    # Windows and Apple hosts are tested with CMAKE_HOST_WIN32 and
    # CMAKE_HOST_APPLE, because OS_NAME is macOS on a Mac and not Darwin.
    if(CMAKE_HOST_WIN32)
        set(suffix "windows.zip")
        set(member "rp6502-emu.exe")
        set(exe "rp6502-emu.exe")
    elseif(CMAKE_HOST_APPLE)
        set(suffix "macos.zip")
        set(member "rp6502-emu.app/Contents/MacOS/rp6502-emu")
    else()
        cmake_host_system_information(RESULT host QUERY OS_NAME)
        cmake_host_system_information(RESULT release QUERY OS_RELEASE)
        cmake_host_system_information(RESULT machine QUERY OS_PLATFORM)
        if(host STREQUAL "Linux")
            # WSL runs the Windows build through interop, which finds a
            # Windows program by its contents and not by .exe.
            if(release MATCHES "[Mm]icrosoft")
                set(suffix "windows.zip")
                set(member "rp6502-emu.exe")
            else()
                set(suffix "linux-${machine}.tar.gz")
            endif()
        endif()
    endif()
    set(RP6502_HOST_SUFFIX "${suffix}" PARENT_SCOPE)
    set(RP6502_HOST_MEMBER "${member}" PARENT_SCOPE)
    set(RP6502_EMU_NAME "${exe}" PARENT_SCOPE)
endfunction()

# Internal helpers of the tool update

# Fetches tools/<name>, and checks the file against <hash> when <hash> is not
# empty. The rename comes last, so a failed fetch leaves the working tool in
# place.
function(_rp6502_fetch_tool name hash)
    set(url "https://raw.githubusercontent.com/${RP6502_TOOLS_REPO}/${RP6502_TOOLS_REF}/tools/${name}")
    set(out "${RP6502_TOOLS_DIR}/${name}")
    message(STATUS "Fetching tools/${name}")
    file(DOWNLOAD "${url}" "${out}.tmp"
        STATUS status
        TLS_VERIFY ON
        INACTIVITY_TIMEOUT 30
    )
    list(GET status 0 code)
    list(GET status 1 text)
    file(SIZE "${out}.tmp" size)
    if(NOT code EQUAL 0 OR size EQUAL 0)
        file(REMOVE "${out}.tmp")
        message(FATAL_ERROR "Cannot fetch ${url}\n${text}")
    endif()
    if(hash)
        file(SHA256 "${out}.tmp" got)
        string(TOLOWER "${hash}" hash)
        if(NOT got STREQUAL hash)
            file(REMOVE "${out}.tmp")
            message(FATAL_ERROR
                "Wrong contents for tools/${name}\n"
                "expected ${hash}\n"
                "     got ${got}")
        endif()
    endif()
    file(RENAME "${out}.tmp" "${out}")
endfunction()

# Sets <out_var> to the name=hash list of tools/SHA256SUMS in the tools
# repository.
function(_rp6502_fetch_sums out_var)
    _rp6502_fetch_tool(SHA256SUMS "")
    _rp6502_read_sums("${RP6502_TOOLS_DIR}/SHA256SUMS" files)
    file(REMOVE "${RP6502_TOOLS_DIR}/SHA256SUMS")
    if(NOT files)
        message(FATAL_ERROR "tools/SHA256SUMS lists nothing to fetch.")
    endif()
    set(${out_var} "${files}" PARENT_SCOPE)
endfunction()

# Fetches the latest release of the emulator for this host, the web player,
# and BASIC, for each one that tools/ has. A configure fetches a tool that
# tools/ lacks when the project uses it.
function(_rp6502_update)
    set(emu)
    if(RP6502_HOST_SUFFIX)
        # An .unsupported record counts too, so the update tries the latest
        # release for this host again.
        file(GLOB emu
            "${RP6502_TOOLS_DIR}/rp6502-*-${RP6502_HOST_SUFFIX}"
            "${RP6502_TOOLS_DIR}/rp6502-*-${RP6502_HOST_SUFFIX}.unsupported")
    endif()
    file(GLOB web "${RP6502_TOOLS_DIR}/rp6502-*-web.zip")
    if(emu OR web)
        _rp6502_latest("${RP6502_TOOLS_REPO}" "^rp6502-(.+)-web\\.zip$" TRUE version)
    endif()
    if(emu)
        _rp6502_fetch_release("${RP6502_TOOLS_REPO}" "${version}"
            "rp6502-${version}-${RP6502_HOST_SUFFIX}" FALSE file)
    endif()
    if(web)
        _rp6502_fetch_release("${RP6502_TOOLS_REPO}" "${version}"
            "rp6502-${version}-web.zip" TRUE file)
    endif()
    file(GLOB found "${RP6502_TOOLS_DIR}/msbasic-*.zip")
    if(found)
        _rp6502_latest("${RP6502_BASIC_REPO}" "^msbasic-(.+)\\.zip$" TRUE version)
        _rp6502_fetch_release("${RP6502_BASIC_REPO}" "${version}"
            "msbasic-${version}.zip" TRUE file)
    endif()
endfunction()

# Adds the update task to .vscode/tasks.json.
function(_rp6502_hook_tasks_json)
    set(file "${RP6502_PROJECT_DIR}/.vscode/tasks.json")
    if(NOT EXISTS "${file}")
        return()
    endif()
    file(READ "${file}" json)
    if(json MATCHES "RP6502: update tools")
        return()
    endif()
    # The task is spliced in as text, because string(JSON) rejects the trailing
    # commas that VS Code accepts and drops every comment on a rewrite.
    set(task [==[

        {
            "label": "RP6502: update tools",
            "type": "process",
            "command": "cmake",
            "args": [
                "-P",
                "${workspaceFolder}/tools/rp6502.cmake"
            ],
            "presentation": {
                "reveal": "always",
                "panel": "dedicated"
            },
            "problemMatcher": []
        },]==])
    string(FIND "${json}" "\"tasks\"" tasks_at)
    if(tasks_at LESS 0)
        message(NOTICE "Add an \"RP6502: update tools\" task to .vscode/tasks.json by hand.")
        return()
    endif()
    string(SUBSTRING "${json}" ${tasks_at} -1 tail)
    string(FIND "${tail}" "[" bracket_at)
    if(bracket_at LESS 0)
        message(NOTICE "Add an \"RP6502: update tools\" task to .vscode/tasks.json by hand.")
        return()
    endif()
    math(EXPR cut "${tasks_at} + ${bracket_at} + 1")
    string(SUBSTRING "${json}" 0 ${cut} head)
    string(SUBSTRING "${json}" ${cut} -1 rest)
    file(WRITE "${file}" "${head}${task}${rest}")
    message(STATUS "Added the update task to .vscode/tasks.json")
endfunction()

# Renames the old entries of .vscode/launch.json, replaces the RP6502-EMU
# entry of older tools, and adds or replaces RP6502-WEB.
function(_rp6502_hook_launch_json)
    set(file "${RP6502_PROJECT_DIR}/.vscode/launch.json")
    if(NOT EXISTS "${file}")
        return()
    endif()
    file(READ "${file}" before)
    string(REPLACE "\"RP6502 (Emulator)\"" "\"RP6502-EMU\"" json "${before}")
    string(REPLACE "\"RP6502 (Hardware)\"" "\"RP6502-PICO\"" json "${json}")
    string(REPLACE "\"RP6502 (Web)\"" "\"RP6502-WEB\"" json "${json}")
    if(NOT json STREQUAL before)
        file(WRITE "${file}" "${json}")
        message(STATUS "Renamed the entries of .vscode/launch.json")
    endif()
    # Older tools wrote an RP6502-EMU entry that starts the emulator through
    # rp6502.py. The new entry starts the rp6502-emu of the launch target.
    set(old_emu [==[
        {
            "name": "RP6502-EMU",
            "type": "lldb-dap",
            "request": "launch",
            "debugAdapterExecutable": "/usr/bin/env",
            "debugAdapterArgs": [
                "python3",
                "${workspaceFolder}/tools/rp6502.py",
                "-c",
                "${workspaceFolder}/.rp6502",
                "emu"
            ],
            "windows": {
                "debugAdapterExecutable": "${env:ComSpec}",
                "debugAdapterArgs": [
                    "/c",
                    "python",
                    "${workspaceFolder}\\tools\\rp6502.py",
                    "-c",
                    "${workspaceFolder}\\.rp6502",
                    "emu"
                ]
            },
            "program": "${command:cmake.launchTargetPath}.rp6502",
            "stopOnEntry": false,
            "stopOnExit": true
        },
]==])
    set(emu [==[
        {
            "name": "RP6502-EMU",
            "type": "lldb-dap",
            "request": "launch",
            "debugAdapterExecutable": "${command:cmake.launchTargetDirectory}/rp6502-emu",
            "debugAdapterArgs": [
                "--dap",
                "--ini",
                "${workspaceFolder}/.rp6502"
            ],
            "windows": {
                "debugAdapterExecutable": "${command:cmake.launchTargetDirectory}/rp6502-emu.exe"
            },
            "program": "${command:cmake.launchTargetPath}.rp6502",
            "args": [],
            "stopOnEntry": false,
            "stopOnExit": true
        },
]==])
    string(FIND "${json}" "${old_emu}" old_at)
    if(old_at GREATER_EQUAL 0)
        string(REPLACE "${old_emu}" "${emu}" json "${json}")
        file(WRITE "${file}" "${json}")
        message(STATUS "Replaced the RP6502-EMU entry of .vscode/launch.json")
    elseif(json MATCHES "\"RP6502-EMU\"" AND NOT json MATCHES "cmake\\.launchTargetDirectory")
        message(NOTICE
            "Change the RP6502-EMU entry of .vscode/launch.json by hand to this:\n${emu}")
    endif()
    # The entry is inserted as text at the end of the configurations, so the
    # entry that F5 starts does not change.
    set(entry [==[
        {
            "name": "RP6502-WEB",
            "type": "debugpy",
            "request": "launch",
            "console": "internalConsole",
            "module": "http.server",
            "args": [
                "--bind",
                "127.0.0.1",
                "--directory",
                "${command:cmake.buildDirectory}/web",
                "0"
            ],
            "env": {
                "PYTHONUNBUFFERED": "1"
            },
            "preLaunchTask": "CMake: build",
            "serverReadyAction": {
                "pattern": "\\((https?://\\S+)\\)",
                "uriFormat": "%s",
                "action": "openExternally"
            },
        },
]==])
    # Older tools wrote an RP6502-WEB entry that runs rp6502.py web. This
    # rp6502.py has no web subcommand, so that entry is replaced.
    set(old_entry [==[
        {
            "name": "RP6502-WEB",
            "type": "debugpy",
            "request": "launch",
            "console": "integratedTerminal",
            "program": "${workspaceFolder}/tools/rp6502.py",
            "args": [
                "web",
                "${command:cmake.launchTargetPath}"
            ],
        },
]==])
    string(FIND "${json}" "${old_entry}" old_at)
    if(old_at GREATER_EQUAL 0)
        string(REPLACE "${old_entry}" "${entry}" json "${json}")
        file(WRITE "${file}" "${json}")
        message(STATUS "Replaced the RP6502-WEB entry of .vscode/launch.json")
    endif()
    if(json MATCHES "\"RP6502-WEB\"")
        return()
    endif()
    # The scan finds the bracket that closes the configurations, outside
    # strings and comments, and the last character before it outside comments.
    set(end_at -1)
    string(FIND "${json}" "\"configurations\"" at)
    if(at GREATER_EQUAL 0)
        string(SUBSTRING "${json}" ${at} -1 tail)
        string(FIND "${tail}" "[" open_at)
        if(open_at GREATER_EQUAL 0)
            math(EXPR last_at "${at} + ${open_at}")
            math(EXPR i "${last_at} + 1")
            string(LENGTH "${json}" length)
            set(depth 1)
            set(state code)
            while(i LESS length)
                string(SUBSTRING "${json}" ${i} 2 pair)
                string(SUBSTRING "${pair}" 0 1 c)
                if(state STREQUAL "string")
                    if(c STREQUAL "\\")
                        math(EXPR i "${i} + 1")
                    elseif(c STREQUAL "\"")
                        set(state code)
                        set(last_at ${i})
                    endif()
                elseif(state STREQUAL "line")
                    if(c STREQUAL "\n")
                        set(state code)
                    endif()
                elseif(state STREQUAL "block")
                    if(pair STREQUAL "*/")
                        set(state code)
                        math(EXPR i "${i} + 1")
                    endif()
                elseif(c STREQUAL "\"")
                    set(state string)
                elseif(pair STREQUAL "//")
                    set(state line)
                elseif(pair STREQUAL "/*")
                    set(state block)
                elseif(c STREQUAL "[")
                    math(EXPR depth "${depth} + 1")
                    set(last_at ${i})
                elseif(c STREQUAL "]")
                    math(EXPR depth "${depth} - 1")
                    if(depth EQUAL 0)
                        set(end_at ${i})
                        break()
                    endif()
                    set(last_at ${i})
                elseif(NOT c MATCHES "^[ \t\r\n]$")
                    set(last_at ${i})
                endif()
                math(EXPR i "${i} + 1")
            endwhile()
        endif()
    endif()
    if(end_at LESS 0)
        message(NOTICE "Add an \"RP6502-WEB\" entry to .vscode/launch.json by hand.")
        return()
    endif()
    math(EXPR cut "${last_at} + 1")
    string(SUBSTRING "${json}" 0 ${cut} head)
    string(SUBSTRING "${json}" ${cut} -1 rest)
    if(head MATCHES "}$")
        string(APPEND head ",")
    endif()
    string(REGEX REPLACE "^[ \t]*\n" "" rest "${rest}")
    if(rest MATCHES "^]")
        string(PREPEND rest "    ")
    endif()
    file(WRITE "${file}" "${head}\n${entry}${rest}")
    message(STATUS "Added the web entry to .vscode/launch.json")
endfunction()

# Replaces the .gitignore lines of the emulator of older tools with lines
# for the emulator, the web player and BASIC.
function(_rp6502_hook_gitignore)
    set(file "${RP6502_PROJECT_DIR}/.gitignore")
    if(NOT EXISTS "${file}")
        return()
    endif()
    file(READ "${file}" before)
    if(NOT before MATCHES "\n$")
        string(APPEND before "\n")
    endif()
    string(REGEX REPLACE "(^|\n)/tools/rp6502-emu\\.\\*\r?\n" "\\1" text "${before}")
    string(REGEX REPLACE "(^|\n)/tools/rp6502-emu(\r?\n)"
        "\\1/tools/rp6502-*\\2/tools/msbasic-*\\2" text "${text}")
    if(NOT text STREQUAL before)
        file(WRITE "${file}" "${text}")
        message(STATUS "Changed the tools lines of .gitignore")
    endif()
endfunction()

# Rewrites each rp6502_map() call of the top CMakeLists.txt as the
# rp6502_align() call that the rp6502_map() stub makes. A call in another
# form keeps the warning of the stub.
function(_rp6502_hook_cmakelists)
    set(file "${RP6502_PROJECT_DIR}/CMakeLists.txt")
    if(NOT EXISTS "${file}")
        return()
    endif()
    file(READ "${file}" before)
    set(text "${before}")
    set(space "[ \t\r\n]")
    set(call "rp6502_map\\(${space}*([A-Za-z0-9_.+-]+)${space}+(\"[^\"]*\"|[^ \t\r\n()\"]+)")
    string(APPEND call "${space}+\"([^\"]*)\"(${space}+\"([^\"]*)\")?${space}*\\)")
    while(text MATCHES "${call}")
        set(old "${CMAKE_MATCH_0}")
        set(target "${CMAKE_MATCH_1}")
        set(regex "${CMAKE_MATCH_3}")
        if(CMAKE_MATCH_4)
            set(regex "(?!(?:${CMAKE_MATCH_5})$)(?:${regex})")
        endif()
        set(new "rp6502_align(${target} \"${regex}\")")
        string(REPLACE "${old}" "${new}" text "${text}")
        message(STATUS "CMakeLists.txt: ${new}")
    endwhile()
    if(NOT text STREQUAL before)
        file(WRITE "${file}" "${text}")
    endif()
endfunction()

# This code runs at include, after every function above is defined.

# cmake -P updates the tools and includes the new file when it changed.
if(CMAKE_SCRIPT_MODE_FILE AND NOT RP6502_TOOLS_RELOADED)
    file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" rp6502_tools_before)
    _rp6502_fetch_sums(rp6502_tools_files)
    foreach(entry IN LISTS rp6502_tools_files)
        string(REGEX MATCH "^(.+)=([0-9a-fA-F]+)$" ignored "${entry}")
        _rp6502_fetch_tool("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
    endforeach()
    set(RP6502_TOOLS_FETCHED TRUE)
    file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" rp6502_tools_after)
    if(NOT rp6502_tools_after STREQUAL rp6502_tools_before)
        set(RP6502_TOOLS_RELOADED TRUE)
        include("${CMAKE_CURRENT_LIST_FILE}")
        return()
    endif()
endif()

_rp6502_host()

# Older tools kept one emulator in tools/, which no version uses.
file(REMOVE
    "${RP6502_TOOLS_DIR}/rp6502-emu"
    "${RP6502_TOOLS_DIR}/rp6502-emu.exe"
    "${RP6502_TOOLS_DIR}/rp6502-emu.unsupported")

# RP6502_TOOLS_FETCHED is set by the update above or by the bootstrap.
if(RP6502_TOOLS_FETCHED)
    _rp6502_hook_tasks_json()
    _rp6502_hook_launch_json()
    _rp6502_hook_gitignore()
    _rp6502_hook_cmakelists()
    _rp6502_update()
endif()

if(CMAKE_SCRIPT_MODE_FILE)
    return()
endif()

# ctest finds the tests of rp6502_test() only when the top folder enables
# testing.
enable_testing()

# The end of the top folder gives each target and test its tools, after
# every setting of a version is made.
get_property(rp6502_deferred GLOBAL PROPERTY RP6502_DEFERRED)
if(NOT rp6502_deferred)
    set_property(GLOBAL PROPERTY RP6502_DEFERRED TRUE)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _rp6502_finish)
endif()
unset(rp6502_deferred)

if(DEFINED CC65_TARGET_SYSTEM)
    find_package(cc65 REQUIRED)
elseif(DEFINED LLVM_MOS_PLATFORM)
    find_package(llvm-mos-sdk REQUIRED)
elseif(NOT RP6502_BASIC)
    message(FATAL_ERROR
        "No compiler selected.\n"
        "Configure with a CMake preset; cmake --list-presets shows them. "
        "Without presets, set CC65_TARGET_SYSTEM or LLVM_MOS_PLATFORM, "
        "or RP6502_BASIC for a BASIC project.")
endif()

# BASIC is a CMake language in every project, so a BASIC program is an
# executable target that cmake.launchTargetPath in VS Code resolves.
set(rp6502_basic_dir "${CMAKE_BINARY_DIR}/CMakeFiles/rp6502-basic")
file(WRITE "${rp6502_basic_dir}/CMakeDetermineBASICCompiler.cmake" [=[
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CMAKE_BASIC_COMPILER "${Python3_EXECUTABLE}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/CMakeBASICCompiler.cmake.in"
    "${CMAKE_PLATFORM_INFO_DIR}/CMakeBASICCompiler.cmake" @ONLY)
set(CMAKE_BASIC_COMPILER_ENV_VAR "")
]=])
file(WRITE "${rp6502_basic_dir}/CMakeBASICCompiler.cmake.in" [=[
set(CMAKE_BASIC_COMPILER "@CMAKE_BASIC_COMPILER@")
set(CMAKE_BASIC_COMPILER_LOADED 1)
set(CMAKE_BASIC_SOURCE_FILE_EXTENSIONS "")
set(CMAKE_BASIC_OUTPUT_EXTENSION .rp6502)
set(CMAKE_BASIC_COMPILER_ENV_VAR "")
]=])
# The executable is an empty file. The ROM is <TARGET>.rp6502, the name that
# a launch configuration makes from the target path, as for C. BASIC comes
# first, so a help asset of the program replaces the help of BASIC.
file(WRITE "${rp6502_basic_dir}/CMakeBASICInformation.cmake"
"set(CMAKE_BASIC_LINK_EXECUTABLE \"<CMAKE_BASIC_COMPILER> \\\"${RP6502_TOOLS_DIR}/rp6502.py\\\" create -o <TARGET>.rp6502 --replace help <LINK_FLAGS> <OBJECTS>\" \"<CMAKE_COMMAND> -E touch <TARGET>\")
set(CMAKE_BASIC_INFORMATION_LOADED 1)
")
file(WRITE "${rp6502_basic_dir}/CMakeTestBASICCompiler.cmake" "set(CMAKE_BASIC_COMPILER_WORKS 1 CACHE INTERNAL \"\")\n")
list(APPEND CMAKE_MODULE_PATH "${rp6502_basic_dir}")
unset(rp6502_basic_dir)
