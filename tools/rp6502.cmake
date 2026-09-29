# The RP6502 project tools: rp6502_executable(), rp6502_asset(),
# rp6502_map(), rp6502_byproducts(), rp6502_basic(), rp6502_web(), and the
# fetch that keeps this directory current.
#
# Update with:  cmake -P tools/rp6502.cmake
#
cmake_minimum_required(VERSION 3.21)

set(RP6502_TOOLS_REPO "picocomputer/rp6502")
set(RP6502_TOOLS_REF "main")
set(RP6502_EMU_RELEASE "latest")
set(RP6502_BASIC_REPO "picocomputer/msbasic")

set(RP6502_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "RP6502 tools directory")
get_filename_component(RP6502_PROJECT_DIR "${RP6502_TOOLS_DIR}" DIRECTORY)

# Where find_package(cc65) looks.
set(cc65_DIR "${RP6502_TOOLS_DIR}")

# Rename over the target, so a dead network leaves the working tool in place.
function(rp6502_fetch_tool name hash)
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

# Both lists this file reads are sha256sum(1) output: the tools in the
# repository and the assets on a release. Returns name=hash pairs.
function(rp6502_read_sums file out_var)
    file(STRINGS "${file}" lines)
    set(entries)
    foreach(line IN LISTS lines)
        if(line MATCHES "^([0-9a-fA-F]+)[ \t]+([^ \t/\\\\]+)$")
            list(APPEND entries "${CMAKE_MATCH_2}=${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set(${out_var} "${entries}" PARENT_SCOPE)
endfunction()

# What to fetch, and what it should hash to, comes from the server.
function(rp6502_fetch_sums out_var)
    rp6502_fetch_tool(SHA256SUMS "")
    rp6502_read_sums("${RP6502_TOOLS_DIR}/SHA256SUMS" files)
    file(REMOVE "${RP6502_TOOLS_DIR}/SHA256SUMS")
    if(NOT files)
        message(FATAL_ERROR "tools/SHA256SUMS lists nothing to fetch.")
    endif()
    set(${out_var} "${files}" PARENT_SCOPE)
endfunction()

# One release archive, reduced to the executable it carries. assets is
# the release's name=hash list. Sets result_var to ok, unsupported when
# the release has no such asset, or failed.
function(rp6502_fetch_emu base assets suffix member exe result_var)
    set(${result_var} failed PARENT_SCOPE)
    set(name)
    foreach(asset IN LISTS assets)
        string(REGEX MATCH "^(.+)=([0-9a-fA-F]+)$" ignored "${asset}")
        # Read out before the next match overwrites them.
        set(asset_name "${CMAKE_MATCH_1}")
        set(asset_hash "${CMAKE_MATCH_2}")
        if(asset_name MATCHES "-${suffix}$")
            set(name "${asset_name}")
            set(hash "${asset_hash}")
        endif()
    endforeach()
    if(NOT name)
        message(NOTICE "No emulator: release ${RP6502_EMU_RELEASE} has no ${suffix}")
        set(${result_var} unsupported PARENT_SCOPE)
        return()
    endif()
    set(tmp "${RP6502_TOOLS_DIR}/${exe}.tmp")
    file(REMOVE_RECURSE "${tmp}")
    file(MAKE_DIRECTORY "${tmp}")
    message(STATUS "Fetching tools/${exe}")
    file(DOWNLOAD "${base}/${name}" "${tmp}/${name}"
        STATUS status
        TLS_VERIFY ON
        INACTIVITY_TIMEOUT 30
    )
    list(GET status 0 code)
    list(GET status 1 text)
    if(NOT code EQUAL 0)
        file(REMOVE_RECURSE "${tmp}")
        message(NOTICE "No emulator: cannot fetch ${base}/${name}\n${text}")
        return()
    endif()
    file(SHA256 "${tmp}/${name}" got)
    string(TOLOWER "${hash}" hash)
    if(NOT got STREQUAL hash)
        file(REMOVE_RECURSE "${tmp}")
        message(NOTICE "No emulator: wrong contents for ${name}")
        return()
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xf "${name}"
        WORKING_DIRECTORY "${tmp}"
        RESULT_VARIABLE result
        ERROR_VARIABLE error
    )
    if(NOT result STREQUAL "0")
        file(REMOVE_RECURSE "${tmp}")
        message(NOTICE "No emulator: cannot unpack ${name}\n${error}")
        return()
    endif()
    file(RENAME "${tmp}/${member}" "${RP6502_TOOLS_DIR}/${exe}" RESULT result)
    file(REMOVE_RECURSE "${tmp}")
    if(NOT result STREQUAL "0")
        message(NOTICE "No emulator: cannot replace tools/${exe}, close it first\n${result}")
        return()
    endif()
    if(NOT CMAKE_HOST_WIN32)
        file(CHMOD "${RP6502_TOOLS_DIR}/${exe}" PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
    endif()
    set(${result_var} ok PARENT_SCOPE)
endfunction()

# Given MISSING, fetches only what tools/ lacks, and nothing at all once
# rp6502-emu.unsupported exists, so a host the release has no build for
# doesn't ask again at every configure. An update removes it and retries.
function(rp6502_fetch_emulator)
    # Each build is suffix|member|exe. OS_NAME is "macOS" on a Mac, not
    # Darwin, so the Windows and Apple hosts are told apart this way.
    set(builds)
    if(CMAKE_HOST_WIN32)
        list(APPEND builds "windows.zip|rp6502-emu.exe|rp6502-emu.exe")
    elseif(CMAKE_HOST_APPLE)
        list(APPEND builds "macos.zip|rp6502-emu.app/Contents/MacOS/rp6502-emu|rp6502-emu")
    else()
        cmake_host_system_information(RESULT host QUERY OS_NAME)
        cmake_host_system_information(RESULT release QUERY OS_RELEASE)
        cmake_host_system_information(RESULT machine QUERY OS_PLATFORM)
        if(host STREQUAL "Linux")
            # WSL runs the Windows build through interop.
            if(release MATCHES "[Mm]icrosoft")
                list(APPEND builds "windows.zip|rp6502-emu.exe|rp6502-emu.exe")
            endif()
            list(APPEND builds "linux-${machine}.tar.gz|rp6502-emu|rp6502-emu")
        endif()
    endif()
    set(sentinel "${RP6502_TOOLS_DIR}/rp6502-emu.unsupported")
    if(NOT "MISSING" IN_LIST ARGN)
        file(REMOVE "${sentinel}")
    elseif(EXISTS "${sentinel}")
        message(STATUS "No emulator: tools/rp6502-emu.unsupported has the reason, "
            "and cmake -P tools/rp6502.cmake tries again.")
        return()
    endif()
    set(assets)
    foreach(build IN LISTS builds)
        string(REPLACE "|" ";" fields "${build}")
        list(GET fields 0 suffix)
        list(GET fields 1 member)
        list(GET fields 2 exe)
        if("MISSING" IN_LIST ARGN AND EXISTS "${RP6502_TOOLS_DIR}/${exe}")
            continue()
        endif()
        # One list serves every build, and it is fetched only when a build
        # is wanted. The timeout bounds a network that drops packets.
        if(NOT assets)
            if(RP6502_EMU_RELEASE STREQUAL "latest")
                set(base "https://github.com/${RP6502_TOOLS_REPO}/releases/latest/download")
            else()
                set(base "https://github.com/${RP6502_TOOLS_REPO}/releases/download/${RP6502_EMU_RELEASE}")
            endif()
            message(STATUS "Fetching the emulator list")
            set(sums "${RP6502_TOOLS_DIR}/rp6502-emu.SHA256SUMS.tmp")
            file(DOWNLOAD "${base}/SHA256SUMS" "${sums}"
                STATUS status
                TLS_VERIFY ON
                TIMEOUT 30
            )
            list(GET status 0 code)
            list(GET status 1 text)
            if(code EQUAL 0)
                rp6502_read_sums("${sums}" assets)
            endif()
            file(REMOVE "${sums}")
            if(NOT assets)
                message(NOTICE "No emulator: cannot fetch ${base}/SHA256SUMS\n${text}")
                break()
            endif()
        endif()
        rp6502_fetch_emu("${base}" "${assets}" "${suffix}" "${member}" "${exe}" result)
        if(result STREQUAL "unsupported")
            file(APPEND "${sentinel}"
                "Release ${RP6502_EMU_RELEASE} has no ${suffix} for tools/${exe}.\n")
        endif()
    endforeach()
endfunction()

# One GitHub request for rp6502_fetch(). Sets <out_var> to ok, missing for
# an answer of 404, or 422 from the commits API for a ref that names no
# commit, failed for any other error answer or a hash that does
# not match [<sha256>], or offline, and <text_var> to the reason. The token
# goes only to api.github.com; curl drops it on the redirect of an artifact
# download to the storage host.
function(rp6502_fetch_url url file out_var text_var)
    set(headers)
    if(url MATCHES "^https://api\\.github\\.com/")
        set(headers
            HTTPHEADER "Accept: application/vnd.github+json"
            HTTPHEADER "X-GitHub-Api-Version: 2022-11-28")
        if(NOT "$ENV{GITHUB_TOKEN}" STREQUAL "")
            list(APPEND headers HTTPHEADER "Authorization: Bearer $ENV{GITHUB_TOKEN}")
        elseif(NOT "$ENV{GH_TOKEN}" STREQUAL "")
            list(APPEND headers HTTPHEADER "Authorization: Bearer $ENV{GH_TOKEN}")
        endif()
    endif()
    file(DOWNLOAD "${url}" "${file}"
        STATUS status
        LOG log
        TLS_VERIFY ON
        INACTIVITY_TIMEOUT 30
        ${headers}
    )
    list(GET status 0 code)
    list(GET status 1 text)
    # The last status line is the answer after any redirects.
    string(REGEX MATCHALL "HTTP/[0-9.]+ [0-9][0-9][0-9]" answers "${log}")
    set(answer)
    if(answers)
        list(GET answers -1 answer)
        string(REGEX REPLACE "^HTTP/[0-9.]+ " "" answer "${answer}")
    endif()
    set(result ok)
    if(NOT code EQUAL 0)
        file(REMOVE "${file}")
        # curl reports an error answer as 22; anything else is the network.
        if(NOT code EQUAL 22)
            set(result offline)
        elseif(answer STREQUAL "404" OR answer STREQUAL "422")
            set(result missing)
        else()
            set(result failed)
            set(text "HTTP ${answer}")
        endif()
    elseif(ARGC GREATER 4)
        file(SHA256 "${file}" got)
        string(TOLOWER "${ARGV4}" expected)
        if(NOT got STREQUAL expected)
            file(REMOVE "${file}")
            set(result failed)
            set(text "wrong contents, SHA256 ${got} where ${expected} was expected")
        endif()
    endif()
    set(${out_var} ${result} PARENT_SCOPE)
    set(${text_var} "${url}: ${text}" PARENT_SCOPE)
endfunction()

# Without a network, the file of the last lookup of a spec is used.
function(rp6502_fetch_last record caller message file_var source_var)
    if(EXISTS "${record}")
        file(STRINGS "${record}" lines)
        list(GET lines 1 file)
        list(GET lines 2 source)
        if(EXISTS "${file}")
            message(NOTICE "${caller}: no network, so the file fetched before is used.")
            set(${file_var} "${file}" PARENT_SCOPE)
            set(${source_var} "${source}" PARENT_SCOPE)
            return()
        endif()
    endif()
    message(FATAL_ERROR "${message}")
endfunction()

# Fetches BASIC or the web zip named by a spec into the build folder, and
# sets <out_var> to the file and <source_var> to where it came from.
# A spec is owner/repo/ref, owner/repo (the latest release), ref (a ref of
# the official repository), or a path ending in .zip or .rp6502. A ref is a
# release tag, or else a commit, whose CI run supplies the file. Tags and
# commits are fetched once per build folder; latest and branches are looked
# up at each configure, and without a network the last lookup is used.
function(rp6502_fetch caller keyword spec out_var source_var)
    if(keyword STREQUAL "BASIC")
        set(repo "${RP6502_BASIC_REPO}")
        set(pattern "^basic\\.rp6502$")
        set(wanted "basic.rp6502")
        set(offline_fix "BASIC tools/basic.rp6502")
    else()
        set(repo "${RP6502_TOOLS_REPO}")
        set(pattern "-web\\.zip$")
        set(wanted "file ending in -web.zip")
        set(offline_fix "EMULATOR tools/rp6502-web.zip")
    endif()
    set(offline_fix "To work offline, commit a copy of the file and name it, such as ${offline_fix}.")
    if(spec MATCHES "\\.(zip|rp6502)$")
        get_filename_component(file "${spec}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${file}" OR IS_DIRECTORY "${file}")
            message(FATAL_ERROR "${caller}: ${keyword} ${spec} is not a file.")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
        set(${out_var} "${file}" PARENT_SCOPE)
        set(${source_var} "file ${file}" PARENT_SCOPE)
        return()
    endif()
    set(ref latest)
    if(spec MATCHES "^([^/]+/[^/]+)(/(.+))?$")
        set(repo "${CMAKE_MATCH_1}")
        if(CMAKE_MATCH_3)
            set(ref "${CMAKE_MATCH_3}")
        endif()
    elseif(NOT spec STREQUAL "")
        set(ref "${spec}")
    endif()
    set(what "${keyword} ${repo}/${ref}")
    set(cache "${CMAKE_BINARY_DIR}/rp6502/${repo}")
    string(MAKE_C_IDENTIFIER "${ref}" key)
    set(record "${cache}/${key}.ref")
    set(tmp "${cache}/${key}.tmp")

    # A tag or a full commit hash names the same file forever.
    if(EXISTS "${record}")
        file(STRINGS "${record}" lines)
        list(GET lines 0 kind)
        list(GET lines 1 file)
        list(GET lines 2 source)
        if(EXISTS "${file}" AND (kind STREQUAL "tag" OR ref MATCHES "^[0-9a-f]{40}$"))
            set(${out_var} "${file}" PARENT_SCOPE)
            set(${source_var} "${source}" PARENT_SCOPE)
            return()
        endif()
    endif()

    # A release: its SHA256SUMS names the file and its hash.
    if(ref STREQUAL "latest")
        set(base "https://github.com/${repo}/releases/latest/download")
    else()
        set(base "https://github.com/${repo}/releases/download/${ref}")
    endif()
    rp6502_fetch_url("${base}/SHA256SUMS" "${tmp}" result text)
    if(result STREQUAL "ok")
        rp6502_read_sums("${tmp}" assets)
        file(REMOVE "${tmp}")
        set(name)
        foreach(asset IN LISTS assets)
            string(REGEX MATCH "^(.+)=([0-9a-fA-F]+)$" ignored "${asset}")
            # Read out before the next match overwrites them.
            set(asset_name "${CMAKE_MATCH_1}")
            set(asset_hash "${CMAKE_MATCH_2}")
            if(asset_name MATCHES "${pattern}")
                set(name "${asset_name}")
                string(TOLOWER "${asset_hash}" hash)
            endif()
        endforeach()
        if(NOT name)
            message(FATAL_ERROR "${caller}: the SHA256SUMS of ${what} lists no ${wanted}.")
        endif()
        set(file "${cache}/${hash}/${name}")
        if(NOT EXISTS "${file}")
            message(STATUS "Fetching ${what}: ${name}")
            rp6502_fetch_url("${base}/${name}" "${file}.tmp" result text ${hash})
            if(NOT result STREQUAL "ok")
                message(FATAL_ERROR "${caller}: cannot fetch ${what}.\n${text}\n${offline_fix}")
            endif()
            file(RENAME "${file}.tmp" "${file}")
        endif()
        if(ref STREQUAL "latest")
            set(kind latest)
        else()
            set(kind tag)
        endif()
        set(source "release ${repo} ${ref}")
    elseif(result STREQUAL "failed")
        message(FATAL_ERROR "${caller}: cannot fetch ${what}.\n${text}")
    elseif(result STREQUAL "offline")
        rp6502_fetch_last("${record}" "${caller}"
            "${caller}: cannot fetch ${what}.\n${text}\n${offline_fix}" file source)
        set(${out_var} "${file}" PARENT_SCOPE)
        set(${source_var} "${source}" PARENT_SCOPE)
        return()
    else()
        # Not a release with SHA256SUMS: a commit, a short hash, or a branch.
        set(api "https://api.github.com/repos/${repo}")
        rp6502_fetch_url("${api}/releases/tags/${ref}" "${tmp}" release text)
        file(REMOVE "${tmp}")
        if(release STREQUAL "ok")
            message(FATAL_ERROR "${caller}: the release ${ref} of ${repo} has no SHA256SUMS.")
        endif()
        if("$ENV{GITHUB_TOKEN}" STREQUAL "" AND "$ENV{GH_TOKEN}" STREQUAL "")
            message(FATAL_ERROR
                "${caller}: ${what} is not a release with SHA256SUMS. A commit is "
                "fetched from its CI run, which needs a GitHub token in GITHUB_TOKEN "
                "or GH_TOKEN.")
        endif()
        rp6502_fetch_url("${api}/commits/${ref}" "${tmp}" result text)
        if(result STREQUAL "missing")
            message(FATAL_ERROR "${caller}: ${repo} has no release or commit named ${ref}.")
        elseif(NOT result STREQUAL "ok")
            rp6502_fetch_last("${record}" "${caller}"
                "${caller}: cannot fetch ${what}.\n${text}\n${offline_fix}" file source)
            set(${out_var} "${file}" PARENT_SCOPE)
            set(${source_var} "${source}" PARENT_SCOPE)
            return()
        endif()
        file(READ "${tmp}" json)
        string(JSON sha GET "${json}" sha)
        set(file)
        file(GLOB found "${cache}/${sha}/*")
        foreach(candidate IN LISTS found)
            get_filename_component(candidate_name "${candidate}" NAME)
            if(candidate_name MATCHES "${pattern}")
                set(file "${candidate}")
            endif()
        endforeach()
        if(file AND EXISTS "${cache}/${sha}/run")
            file(READ "${cache}/${sha}/run" source)
        else()
            rp6502_fetch_url("${api}/actions/runs?head_sha=${sha}&per_page=100"
                "${tmp}" result text)
            if(NOT result STREQUAL "ok")
                message(FATAL_ERROR "${caller}: cannot fetch ${what}.\n${text}\n${offline_fix}")
            endif()
            file(READ "${tmp}" json)
            string(JSON count LENGTH "${json}" workflow_runs)
            # Pushed and dispatched runs build the commit itself; a pull
            # request run builds its merge with the base branch.
            set(first)
            set(later)
            if(count GREATER 0)
                math(EXPR last "${count} - 1")
                foreach(i RANGE ${last})
                    string(JSON id GET "${json}" workflow_runs ${i} id)
                    string(JSON event GET "${json}" workflow_runs ${i} event)
                    if(event STREQUAL "pull_request")
                        list(APPEND later ${id})
                    else()
                        list(APPEND first ${id})
                    endif()
                endforeach()
            endif()
            set(runs ${first} ${later})
            set(source)
            foreach(run IN LISTS runs)
                rp6502_fetch_url("${api}/actions/runs/${run}/artifacts?per_page=100" "${tmp}" result text)
                if(NOT result STREQUAL "ok")
                    continue()
                endif()
                file(READ "${tmp}" json)
                string(JSON count LENGTH "${json}" artifacts)
                if(count EQUAL 0)
                    continue()
                endif()
                math(EXPR last "${count} - 1")
                foreach(i RANGE ${last})
                    string(JSON name GET "${json}" artifacts ${i} name)
                    string(JSON expired GET "${json}" artifacts ${i} expired)
                    if(NOT name MATCHES "${pattern}" OR expired)
                        continue()
                    endif()
                    string(JSON id GET "${json}" artifacts ${i} id)
                    string(JSON digest ERROR_VARIABLE no_digest GET "${json}" artifacts ${i} digest)
                    set(check)
                    if(NOT no_digest AND digest MATCHES "^sha256:([0-9a-f]+)$")
                        set(check ${CMAKE_MATCH_1})
                    endif()
                    set(file "${cache}/${sha}/${name}")
                    message(STATUS "Fetching ${what}: ${name} from CI run ${run}")
                    rp6502_fetch_url("${api}/actions/artifacts/${id}/zip" "${file}.tmp" result text ${check})
                    if(NOT result STREQUAL "ok")
                        message(FATAL_ERROR "${caller}: cannot fetch ${what}.\n${text}\n${offline_fix}")
                    endif()
                    file(RENAME "${file}.tmp" "${file}")
                    set(source "run ${repo} ${run}")
                    file(WRITE "${cache}/${sha}/run" "${source}")
                    break()
                endforeach()
                if(source)
                    break()
                endif()
            endforeach()
            file(REMOVE "${tmp}")
            if(NOT source)
                message(FATAL_ERROR
                    "${caller}: the CI runs of ${repo} commit ${sha} have no "
                    "${wanted} that has not expired. Artifacts expire after 90 days.")
            endif()
        endif()
        set(kind commit)
    endif()
    file(WRITE "${record}" "${kind}\n${file}\n${source}\n")
    set(${out_var} "${file}" PARENT_SCOPE)
    set(${source_var} "${source}" PARENT_SCOPE)
endfunction()

# Hooks patch config files.
function(rp6502_hook_tasks_json)
    set(file "${RP6502_PROJECT_DIR}/.vscode/tasks.json")
    if(NOT EXISTS "${file}")
        return()
    endif()
    file(READ "${file}" json)
    if(json MATCHES "RP6502: update tools")
        return()
    endif()
    # Spliced as text, not through string(JSON), which rejects the trailing
    # commas VS Code allows and drops every comment on rewrite.
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

function(rp6502_hook_launch_json)
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
    if(json MATCHES "\"RP6502-WEB\"")
        return()
    endif()
    # Spliced as text, as in rp6502_hook_tasks_json(), at the end of the
    # configurations, so the entry selected for F5 stays the same.
    set(entry [==[
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
    # The bracket that closes the configurations, found by counting
    # brackets outside strings and comments. The entry goes after the last
    # character outside comments, with a comma when that character closes
    # an entry.
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

if(CMAKE_SCRIPT_MODE_FILE AND NOT RP6502_TOOLS_RELOADED)
    file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" rp6502_tools_before)
    rp6502_fetch_sums(rp6502_tools_files)
    foreach(entry IN LISTS rp6502_tools_files)
        string(REGEX MATCH "^(.+)=([0-9a-fA-F]+)$" ignored "${entry}")
        rp6502_fetch_tool("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
    endforeach()
    set(RP6502_TOOLS_FETCHED TRUE)
    file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" rp6502_tools_after)
    if(NOT rp6502_tools_after STREQUAL rp6502_tools_before)
        set(RP6502_TOOLS_RELOADED TRUE)
        include("${CMAKE_CURRENT_LIST_FILE}")
        return()
    endif()
endif()

if(RP6502_TOOLS_FETCHED)
    rp6502_hook_tasks_json()
    rp6502_hook_launch_json()
    rp6502_fetch_emulator()
endif()

if(CMAKE_SCRIPT_MODE_FILE)
    return()
endif()

# A clone has the tools but not the emulator, which git ignores. The
# bootstrap's first configure has just fetched everything.
if(NOT RP6502_TOOLS_FETCHED)
    rp6502_fetch_emulator(MISSING)
endif()

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

# BASIC is a language to CMake, so a BASIC program is an executable target
# like a C one, and cmake.launchTargetPath in VS Code finds it the same way.
# Written for every project, because any project can list BASIC in
# project() beside C and ASM.
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
# The executable is an empty file. The ROM is <TARGET>.rp6502 beside it,
# the name a launch configuration makes from the target path, as for C.
# BASIC comes first, so a help asset of the program replaces the help of
# BASIC.
file(WRITE "${rp6502_basic_dir}/CMakeBASICInformation.cmake"
"set(CMAKE_BASIC_LINK_EXECUTABLE \"<CMAKE_BASIC_COMPILER> \\\"${RP6502_TOOLS_DIR}/rp6502.py\\\" -o <TARGET>.rp6502 create --replace help <LINK_FLAGS> <OBJECTS>\" \"<CMAKE_COMMAND> -E touch <TARGET>\")
set(CMAKE_BASIC_INFORMATION_LOADED 1)
")
file(WRITE "${rp6502_basic_dir}/CMakeTestBASICCompiler.cmake" "set(CMAKE_BASIC_COMPILER_WORKS 1 CACHE INTERNAL \"\")\n")
list(APPEND CMAKE_MODULE_PATH "${rp6502_basic_dir}")
unset(rp6502_basic_dir)

# cc65 links a flat image at a fixed address;
# llvm-mos writes the address into the start of its output file.
function(rp6502_default_address var)
    if(CMAKE_C_COMPILER_ID STREQUAL "cc65")
        set(${var} 0x200 PARENT_SCOPE)
    else()
        set(${var} file PARENT_SCOPE)
    endif()
endfunction()

# Package the target as an RP6502 ROM.
#
# RP6502 Executable ROM
# ^^^^^^^^^^^^^^^^^^^^^
#
#  rp6502_executable(<name>
#                    [DATA <addr>]
#                    [NMI <addr>]
#                    [RESET <addr>]
#                    [IRQ <addr>]
#                    roms...)
#
# Packages the executable produced by ``<name>`` into RP6502 ROM format.
# Merges with specified ``roms...`` and those created with rp6502_asset().
# When ``DATA`` is omitted, the linker output of ``<name>`` is not
# included in the merge; only the asset/extra ROMs are bundled.
# The word `file` may be used for any <addr> indicating the
# address is to be read from the linker output in this order:
# ``DATA <addr>`` Starting memory address to load data.
# ``NMI <addr>`` Address for NMI to be stored at $FFFA-$FFFB.
# ``RESET <addr>`` Address for RESET to be stored at $FFFC-$FFFD.
# ``IRQ <addr>`` Address for IRQ to be stored at $FFFE-$FFFF.
# The word `default` may be used for any <addr> to take the compiler's
# own convention for where its linker output loads.
#
function(rp6502_executable name)
    # Parse args
    set(data_addr "none")
    set(nmi_addr "none")
    set(reset_addr "none")
    set(irq_addr "none")
    set(extra_roms)
    foreach(X IN LISTS ARGN)
        if (NOT data_addr)
            set(data_addr ${X})
        elseif (NOT reset_addr)
            set(reset_addr ${X})
        elseif (NOT irq_addr)
            set(irq_addr ${X})
        elseif (NOT nmi_addr)
            set(nmi_addr ${X})
        elseif (X STREQUAL "DATA")
            set(data_addr FALSE)
        elseif (X STREQUAL "RESET")
            set(reset_addr FALSE)
        elseif (X STREQUAL "IRQ")
            set(irq_addr FALSE)
        elseif (X STREQUAL "NMI")
            set(nmi_addr FALSE)
        else ()
            list(APPEND extra_roms ${X})
        endif ()
    endforeach()
    rp6502_default_address(default_addr)
    foreach(V data_addr nmi_addr reset_addr irq_addr)
        if (${V} STREQUAL "default")
            set(${V} "${default_addr}")
        endif()
    endforeach()
    # Resolve relative extra_roms against current source dir
    set(all_extra_roms)
    foreach(rom IN LISTS extra_roms)
        if (IS_ABSOLUTE "${rom}")
            list(APPEND all_extra_roms "${rom}")
        else()
            list(APPEND all_extra_roms "${CMAKE_CURRENT_SOURCE_DIR}/${rom}")
        endif()
    endforeach()
    # Collect asset ROMs registered by rp6502_asset()
    get_target_property(asset_roms ${name} RP6502_ASSET_ROMS)
    if (asset_roms)
        list(APPEND all_extra_roms ${asset_roms})
    endif()
    # Build the rp6502.py merge command
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(rom_file "${CMAKE_CURRENT_BINARY_DIR}/${name}.rp6502")
    set(tool_command "${Python3_EXECUTABLE}"
        "${RP6502_TOOLS_DIR}/rp6502.py"
    )
    set(executable_inputs)
    if (NOT data_addr STREQUAL "none")
        list(APPEND tool_command -a "${data_addr}")
        list(APPEND executable_inputs "$<TARGET_FILE:${name}>")
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
        create ${executable_inputs}
        -- ${all_extra_roms}
    )
    # The ROM is its own buildable artifact, depending on the executable
    # (when DATA is given) plus every asset rom.
    add_custom_command(
        OUTPUT "${rom_file}"
        DEPENDS ${executable_inputs} ${all_extra_roms}
        COMMAND ${CMAKE_COMMAND} -E rm -f "${rom_file}"
        COMMAND ${tool_command}
        VERBATIM
    )
    add_custom_target(${name}_rp6502 ALL DEPENDS "${rom_file}")
    # A layout that fails its checks must not produce a ROM.
    get_target_property(map_checks ${name} RP6502_MAP_CHECKS)
    if (map_checks)
        add_dependencies(${name}_rp6502 ${map_checks})
    endif()
    # Mark that rp6502_executable has been called for this target
    set_property(TARGET ${name} PROPERTY RP6502_EXECUTABLE_CALLED TRUE)
    set_target_properties(${name} PROPERTIES
        RP6502_ROM "${rom_file}"
        RP6502_ROM_TARGET ${name}_rp6502)
endfunction()

# Package anything as an RP6502 asset ROM.
#
# RP6502 Asset ROM
# ^^^^^^^^^^^^^^^^
#
#  rp6502_asset(<name> <address> <in_file>)
#
# If the address is numeric, the in_file will be loaded into
# RAM ($0-FFFF) or XRAM ($10000-1FFFF) when the ROM is loaded.
# Non-numeric addresses become filenames that can be opened
# with "ROM:filename" from a micro filesystem in the ROM.
# Writing the address as RAM(<x>) or XRAM(<x>) checks that it is in range,
# and XRAM() sets the bit that tells XRAM from RAM, so an offset from
# rp6502_map() loads into XRAM. Inside the parentheses, <x> is a number,
# a name rp6502_map() read for this target, or the name of a CMake
# variable.
#
function(rp6502_asset name)
    get_target_property(executable_called ${name} RP6502_EXECUTABLE_CALLED)
    if (executable_called)
        message(FATAL_ERROR
            "rp6502_asset(${name} ...) must be registered BEFORE calling rp6502_executable() or rp6502_basic()."
        )
    endif()
    # CMake gives every parenthesis to a command as an argument of its own,
    # so RAM(<x>) arrives here as four arguments and nothing named RAM or
    # XRAM is ever defined.
    set(args ${ARGN})
    list(LENGTH args argc)
    list(GET args 0 addr)
    set(unread FALSE)
    if (addr STREQUAL "RAM" OR addr STREQUAL "XRAM")
        set(form "${addr}")
        if (NOT argc EQUAL 5)
            message(FATAL_ERROR "rp6502_asset(${name} ${form}(<address>) <in_file>)")
        endif()
        list(GET args 1 opened)
        list(GET args 3 closed)
        if (NOT opened STREQUAL "(" OR NOT closed STREQUAL ")")
            message(FATAL_ERROR "rp6502_asset(${name} ${form}(<address>) <in_file>)")
        endif()
        list(GET args 2 value)
        set(token "${value}")
        get_target_property(read ${name} RP6502_MAP_NAME_${value})
        if (NOT read MATCHES "-NOTFOUND$")
            set(value "${read}")
        elseif (DEFINED ${value})
            set(value "${${value}}")
        endif()
        set(written "${value}")
        # rp6502_map() gives an address it could not read this value.
        if (value STREQUAL "0xFFFFFFFF")
            set(unread TRUE)
            # Stands in until the build refuses it.
            set(value 0)
        endif()
        # A leading $ is how a 6502 program writes hex, which rp6502.py
        # takes as well.
        string(REGEX REPLACE "^\\$" "0x" value "${value}")
        if (NOT value MATCHES "^[-+]?(0[xX][0-9a-fA-F]+|[0-9]+)$")
            message(FATAL_ERROR
                "rp6502_asset(${name} ${form}(...)): ${written} is not a number,"
                " or a name from rp6502_map(${name} ...).")
        endif()
        if (form STREQUAL "RAM")
            set(limit 65535)
            set(ends "0xFFFF")
        else()
            set(limit 131071)
            set(ends "0x1FFFF")
        endif()
        math(EXPR value "(${value})")
        if (value LESS 0 OR value GREATER ${limit})
            message(FATAL_ERROR
                "rp6502_asset(${name} ${form}(...)): ${written} is outside ${form}, which ends at ${ends}.")
        endif()
        if (form STREQUAL "XRAM")
            math(EXPR value "${value} | 0x10000")
        endif()
        math(EXPR addr "${value}" OUTPUT_FORMAT HEXADECIMAL)
        if (unread)
            # Keeps two refusals from sharing an output.
            set(addr "${form}_${token}")
        endif()
        list(GET args 4 in_file)
    else()
        if (NOT argc EQUAL 2)
            message(FATAL_ERROR "rp6502_asset(<name> <address> <in_file>)")
        endif()
        list(GET args 1 in_file)
    endif()
    get_filename_component(src_file "${in_file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    file(RELATIVE_PATH rel_path "${CMAKE_SOURCE_DIR}" "${src_file}")
    if (rel_path MATCHES "^\\.\\.")
        get_filename_component(rel_path "${src_file}" NAME)
    endif()
    string(MAKE_C_IDENTIFIER "${addr}" key)
    set(out_file "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${name}.rp6502/${key}/${rel_path}")
    get_filename_component(out_dir "${out_file}" DIRECTORY)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(create
        COMMAND "${Python3_EXECUTABLE}"
                "${RP6502_TOOLS_DIR}/rp6502.py"
                -a "${addr}"
                -o "${out_file}"
                create "${src_file}")
    # The configure has to finish for the build to report why the address
    # was not read, so the refusal waits for the build too.
    if (unread)
        set(create
            COMMAND ${CMAKE_COMMAND} -E echo
                "rp6502_asset(${name} ${form}(${token})): rp6502_map() did not read this address, or it does not fit in 16 bits."
            COMMAND ${CMAKE_COMMAND} -E false)
    endif()
    add_custom_command(
        OUTPUT "${out_file}"
        DEPENDS "${src_file}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        ${create}
        VERBATIM
    )
    set_property(TARGET ${name} APPEND PROPERTY
        RP6502_ASSET_ROMS "${out_file}"
    )
    set_property(TARGET ${name} APPEND PROPERTY RP6502_ASSET_NAMES "${addr}")
endfunction()

# Package BASIC programs with BASIC.
#
# RP6502 BASIC
# ^^^^^^^^^^^^
#
#  rp6502_basic(<name> [BASIC <spec>] [<autorun>])
#
# Builds <name>.rp6502 from BASIC and the assets of the executable target
# <name>, which rp6502_asset() adds before this call, such as
# rp6502_asset(<name> game.bas src/game.bas). <autorun> is the name of
# the asset that BASIC runs at start, through an asset autorun.bas written
# here; without it, BASIC starts at its prompt. One program starts another
# with RUN "ROM:<name>". The project lists BASIC in project(). A project
# with no compiler, such as project(<name> BASIC), sets RP6502_BASIC in
# the configure preset.
# ``BASIC <spec>`` names the BASIC ROM: owner/repo/ref, owner/repo (its
# latest release), a ref of picocomputer/msbasic, or a .rp6502 file of
# the project. The default is the latest release of picocomputer/msbasic.
#
function(rp6502_basic name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "BASIC" "")
    set(caller "rp6502_basic(${name})")
    list(LENGTH arg_UNPARSED_ARGUMENTS count)
    if (count GREATER 1 OR NOT TARGET ${name})
        message(FATAL_ERROR
            "rp6502_basic(<name> [BASIC <spec>] [<autorun>]), after add_executable(<name>) "
            "and its rp6502_asset() calls")
    endif()
    if (NOT CMAKE_BASIC_COMPILER_LOADED)
        message(FATAL_ERROR
            "${caller}: BASIC is not a language of the project. List it in "
            "project(), as in project(<name> BASIC) or project(<name> C ASM BASIC).")
    endif()
    # tools/basic.rp6502 was the BASIC of a project before specs. It is
    # never taken by default, so the configure stops for a project that
    # relied on it.
    if (NOT arg_BASIC AND EXISTS "${RP6502_TOOLS_DIR}/basic.rp6502")
        message(FATAL_ERROR
            "${caller}: tools/basic.rp6502 is no longer used by default. Name it "
            "with rp6502_basic(${name} BASIC tools/basic.rp6502 ...), or delete it "
            "for the latest release.")
    endif()
    rp6502_fetch("${caller}" BASIC "${arg_BASIC}" basic source)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${name}.basic")
    # Rewritten only when BASIC changes, so a Makefile relinks for a new
    # BASIC that is older than the ROM.
    file(CONFIGURE OUTPUT "${dir}/basic.txt" CONTENT "${basic}\n")
    if (count EQUAL 1)
        # The ROM: drive ignores case, and BASIC is written in capitals.
        string(TOUPPER "${arg_UNPARSED_ARGUMENTS}" autorun)
        get_target_property(names ${name} RP6502_ASSET_NAMES)
        string(TOUPPER "${names}" names)
        if (NOT autorun IN_LIST names)
            message(FATAL_ERROR
                "${caller}: no rp6502_asset(${name} ${arg_UNPARSED_ARGUMENTS} ...) "
                "comes before it.")
        endif()
        # Written only when it changes, so a configure does not rebuild the ROM.
        file(CONFIGURE OUTPUT "${dir}/autorun.bas" CONTENT "10 RUN \"ROM:${autorun}\"\n")
        rp6502_asset(${name} autorun.bas "${dir}/autorun.bas")
    endif()
    get_target_property(assets ${name} RP6502_ASSET_ROMS)
    if (NOT assets)
        set(assets)
    endif()
    # As sources, the assets are built before the link that merges them.
    target_sources(${name} PRIVATE ${assets})
    set(rom "${CMAKE_CURRENT_BINARY_DIR}/${name}.rp6502")
    set_target_properties(${name} PROPERTIES LINKER_LANGUAGE BASIC SUFFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    # The link writes the ROM, and Ninja needs to be told which rule does.
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E true
        BYPRODUCTS "${rom}"
        VERBATIM)
    target_link_options(${name} PRIVATE "${basic}" ${assets})
    set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS
        "${basic}" "${dir}/basic.txt" ${assets})
    set_target_properties(${name} PROPERTIES
        RP6502_EXECUTABLE_CALLED TRUE
        RP6502_ROM "${rom}"
        RP6502_ROM_TARGET ${name})
endfunction()

# Package a ROM as a web page.
#
# RP6502 Web
# ^^^^^^^^^^
#
#  rp6502_web(<rom> [OUTPUT <name>.zip] [EMULATOR <spec>]
#             [PAGE <file> | <folder>] [CONFIG <text>])
#
# Builds web/<name>.zip in the build folder, by default <rom>.zip, with
# the same files unpacked in web/<name>/, from the target <rom> of
# rp6502_executable() or rp6502_basic(). At the root are the ROM, and
# rp6502.js and rp6502.wasm from the web zip that EMULATOR names:
# owner/repo/ref, owner/repo (its latest release), a ref of
# picocomputer/rp6502, or a .zip file of the project. The default is the
# latest release of picocomputer/rp6502, and RP6502_WEB_EMULATOR, when it
# is set, replaces EMULATOR in every call. PAGE is the page file, stored
# as index.html, or a folder copied with its subfolders, whose root
# index.html is the page. Without one, the page is the index.html of the
# web zip. CONFIG is JavaScript, the keys and values of an object: its
# keys replace the same keys in CONFIG of the page, and add the others.
# CONFIG.rom is always the ROM, and CONFIG.github, for the links under a
# footer, is the GitHub repository of the git remote origin unless CONFIG
# names another.
#
function(rp6502_web rom)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "OUTPUT;EMULATOR;PAGE;CONFIG" "")
    set(caller "rp6502_web(${rom})")
    if (arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "${caller}: ${arg_UNPARSED_ARGUMENTS} follows no keyword. rp6502_web(<rom> "
            "[OUTPUT <name>.zip] [EMULATOR <spec>] [PAGE <file> | <folder>] [CONFIG <text>])")
    endif()
    if (TARGET ${rom})
        get_target_property(rom_file ${rom} RP6502_ROM)
        get_target_property(rom_target ${rom} RP6502_ROM_TARGET)
    endif()
    if (NOT rom_file)
        message(FATAL_ERROR
            "${caller}: ${rom} makes no ROM. Call rp6502_executable(${rom} ...) "
            "or rp6502_basic(${rom} ...) before rp6502_web(${rom} ...).")
    endif()
    set(zip "${rom}.zip")
    if (arg_OUTPUT)
        set(zip "${arg_OUTPUT}")
    endif()
    if (NOT zip MATCHES "^[A-Za-z0-9_-][A-Za-z0-9._-]*\\.zip$")
        message(FATAL_ERROR "${caller}: OUTPUT ${zip} is not a file name ending in .zip.")
    endif()
    string(REGEX REPLACE "\\.zip$" "" name "${zip}")
    if (TARGET ${name}_web)
        message(FATAL_ERROR "${caller}: two rp6502_web() calls make ${zip}.")
    endif()
    set(spec "${arg_EMULATOR}")
    if (RP6502_WEB_EMULATOR)
        set(spec "${RP6502_WEB_EMULATOR}")
    endif()
    rp6502_fetch("${caller}" EMULATOR "${spec}" web_zip source)

    set(dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${name}.web")
    file(REMOVE_RECURSE "${dir}/emulator")
    file(ARCHIVE_EXTRACT INPUT "${web_zip}" DESTINATION "${dir}/emulator"
        PATTERNS rp6502.js rp6502.wasm index.html)
    foreach(part rp6502.js rp6502.wasm index.html)
        if (NOT EXISTS "${dir}/emulator/${part}")
            message(FATAL_ERROR "${caller}: ${web_zip} has no ${part} at its root.")
        endif()
    endforeach()

    set(page "${dir}/emulator/index.html")
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
            if (relative STREQUAL "${rom}.rp6502" OR relative STREQUAL "rp6502.js"
                    OR relative STREQUAL "rp6502.wasm")
                message(FATAL_ERROR "${caller}: ${arg_PAGE}/${relative} has the name of a file rp6502_web() makes.")
            endif()
            list(APPEND folder_files "${file}")
        endforeach()
        file(GLOB folder_entries LIST_DIRECTORIES true RELATIVE "${folder}" "${folder}/*")
        list(REMOVE_ITEM folder_entries index.html)
    endif()
    if (NOT page STREQUAL "${dir}/emulator/index.html")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${page}")
    endif()

    # The settings run in a script of their own, before rp6502.js reads CONFIG.
    file(READ "${page}" html)
    string(FIND "${html}" "<script src=\"rp6502.js\"" at)
    if (at LESS 0)
        message(FATAL_ERROR "${caller}: ${page} has no <script src=\"rp6502.js\">.")
    endif()
    set(script "<script>\n")
    if (page STREQUAL "${dir}/emulator/index.html")
        string(APPEND script "CONFIG.title = '';\n")
    endif()
    # The footer links to the GitHub repository that the git remote of the
    # project names; CONFIG can name another.
    execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}" remote get-url origin
        OUTPUT_VARIABLE origin OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (origin MATCHES "github\\.com[:/]([^/]+/[^/]+)$")
        string(REGEX REPLACE "\\.git$" "" repo "${CMAKE_MATCH_1}")
        string(APPEND script "CONFIG.github = '${repo}';\n")
    endif()
    if (DEFINED arg_CONFIG)
        string(APPEND script "Object.assign(CONFIG, {\n${arg_CONFIG}\n});\n")
    endif()
    string(APPEND script "CONFIG.rom = '${rom}.rp6502';\n</script>\n")
    string(SUBSTRING "${html}" 0 ${at} head)
    string(SUBSTRING "${html}" ${at} -1 tail)
    set(html "${head}${script}${tail}")
    # Written only when it changes, so a configure does not rebuild the zip.
    set(written)
    if (EXISTS "${dir}/index.html")
        file(READ "${dir}/index.html" written)
    endif()
    if (NOT written STREQUAL html)
        file(WRITE "${dir}/index.html" "${html}")
    endif()
    file(SHA256 "${web_zip}" web_hash)
    string(REPLACE ";" "\n" listed "${folder_files}")
    file(CONFIGURE OUTPUT "${dir}/sources.txt"
        CONTENT "${web_zip} ${web_hash}\n${listed}\n" @ONLY)
    set(out "${CMAKE_BINARY_DIR}/web")
    file(CONFIGURE OUTPUT "${out}/${name}.emulator" CONTENT "${source}\n" @ONLY)

    set(stage "${out}/${name}")
    set(copy_folder)
    if (folder)
        set(copy_folder COMMAND "${CMAKE_COMMAND}" -E copy_directory "${folder}" "${stage}")
    endif()
    add_custom_command(
        OUTPUT "${out}/${zip}"
        DEPENDS "${rom_file}" "${dir}/index.html" "${dir}/sources.txt" ${folder_files}
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${stage}" "${out}/${zip}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${stage}"
        ${copy_folder}
        COMMAND "${CMAKE_COMMAND}" -E copy
            "${dir}/emulator/rp6502.js" "${dir}/emulator/rp6502.wasm" "${dir}/index.html"
            "${stage}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${rom_file}" "${stage}/${rom}.rp6502"
        COMMAND "${CMAKE_COMMAND}" -E chdir "${stage}"
            "${CMAKE_COMMAND}" -E tar cf "${out}/${zip}" --format=zip --
            index.html rp6502.js rp6502.wasm ${rom}.rp6502 ${folder_entries}
        COMMENT "Packaging web/${zip}"
        VERBATIM
    )
    add_custom_target(${name}_web ALL DEPENDS "${out}/${zip}")
    add_dependencies(${name}_web ${rom_target})
endfunction()

# Give CMake the addresses a header defines.
#
# RP6502 Memory Map
# ^^^^^^^^^^^^^^^^^
#
#  rp6502_map(<target> <header> <regex> [<unaligned_regex>])
#
# Reads the ``#define`` lines of ``<header>`` whose name matches
# ``<regex>`` and records each name on ``<target>`` with the value the
# header computes. The names then work inside RAM() and XRAM() in that
# target's rp6502_asset() calls, and its ROM waits for the header's check.
# Names matching ``<unaligned_regex>`` are exempt from 16-bit alignment.
# A target can read several headers, one call each, and a name defined
# by two of them stops the configure. A commented out define, a define
# with no value, and a function-like macro are all skipped.
#
# The layout is checked while the project builds rather than while it
# configures, so a header that will not compile still leaves a configured
# project behind, and every problem is reported by the compiler against
# the line in the header.
#
function(rp6502_map target)
    # The arguments are counted here rather than named, so a wrong count
    # gets the usage instead of CMake's complaint about the call.
    if (ARGC LESS 3 OR ARGC GREATER 4)
        message(FATAL_ERROR
            "rp6502_map(<target> <header> <regex> [<unaligned_regex>])")
    endif()
    if (NOT TARGET ${target})
        message(FATAL_ERROR
            "rp6502_map(${target} ...): ${target} is not a target. "
            "Call rp6502_map() after add_executable(${target}).")
    endif()
    set(header "${ARGV1}")
    set(regex "${ARGV2}")
    set(unaligned)
    if (ARGC EQUAL 4)
        set(unaligned "${ARGV3}")
    endif()
    get_target_property(executable_called ${target} RP6502_EXECUTABLE_CALLED)
    if (executable_called)
        message(FATAL_ERROR
            "rp6502_map(${target} ...) must be registered BEFORE calling rp6502_executable()."
        )
    endif()
    get_filename_component(header_file "${header}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(header_name "${header_file}" NAME)
    get_filename_component(header_dir "${header_file}" DIRECTORY)
    # Editing the layout has to configure again, since these values are read
    # at configure time.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${header_file}")

    # Read the header a line at a time. file(STRINGS), and every idiom that
    # escapes the text and splits it, join a line ending in a backslash to
    # the line after it, which moves every line number that follows.
    # Nothing here reads comments or conditionals, because each name is
    # written out under an #ifdef and the preprocessor settles what exists.
    file(READ "${header_file}" rest)
    string(REPLACE "\r\n" "\n" rest "${rest}")
    set(names)
    set(lines)
    set(values)
    set(lineno 0)
    set(pending "")
    set(pending_line 0)
    while(TRUE)
        string(FIND "${rest}" "\n" pos)
        if (pos LESS 0)
            set(line "${rest}")
            set(rest "")
            set(last TRUE)
        else()
            string(SUBSTRING "${rest}" 0 ${pos} line)
            math(EXPR pos "${pos}+1")
            string(SUBSTRING "${rest}" ${pos} -1 rest)
            set(last FALSE)
        endif()
        math(EXPR lineno "${lineno}+1")

        # A continued definition is read under the number of its first line.
        if (pending STREQUAL "")
            set(cur "${line}")
            set(cur_line ${lineno})
        else()
            set(cur "${pending}${line}")
            set(cur_line ${pending_line})
        endif()
        if (cur MATCHES "\\\\$")
            string(REGEX REPLACE "\\\\$" " " pending "${cur}")
            set(pending_line ${cur_line})
        else()
            set(pending "")
            if (cur MATCHES "^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)([^A-Za-z0-9_(].*)$")
                set(name "${CMAKE_MATCH_1}")
                string(STRIP "${CMAKE_MATCH_2}" value)
                # An include guard has no value and only integers are carried.
                if (NOT value STREQUAL "" AND NOT value MATCHES "^[\"']"
                        AND name MATCHES "^(${regex})$")
                    list(APPEND names "${name}")
                    list(APPEND lines "${cur_line}")
                    list(APPEND values "${value}")
                endif()
            endif()
        endif()
        if (last)
            break()
        endif()
    endwhile()

    # Every structure an offsetof names is probed below, whatever else is
    # written around it, and the first name that uses one carries its line.
    set(probes)
    set(probe_lines)
    set(probe_guards)
    foreach(name value line IN ZIP_LISTS names values lines)
        if (value MATCHES "offsetof[ \t]*\\(([^,]+),")
            string(STRIP "${CMAKE_MATCH_1}" type)
            list(FIND probes "${type}" index)
            if (index LESS 0)
                list(APPEND probes "${type}")
                list(APPEND probe_lines "${line}")
                list(APPEND probe_guards "defined(${name})")
            else()
                # Any one of them being defined is enough to probe with.
                list(GET probe_guards ${index} guard)
                list(REMOVE_AT probe_guards ${index})
                list(INSERT probe_guards ${index} "${guard} || defined(${name})")
            endif()
        endif()
    endforeach()

    # Two targets can share a header, and a target can read two headers
    # with the same file name, so the files are kept apart by both.
    file(RELATIVE_PATH id "${CMAKE_SOURCE_DIR}" "${header_file}")
    if (id MATCHES "^\\.\\.")
        # A whole absolute path would push the ROM path past what the
        # emulator opens.
        get_filename_component(stem "${header_file}" NAME_WE)
        string(SHA1 hash "${header_file}")
        string(SUBSTRING "${hash}" 0 8 hash)
        set(id "${stem}_${hash}")
    endif()
    string(MAKE_C_IDENTIFIER "${id}" id)
    if (TARGET ${target}_map_${id})
        message(FATAL_ERROR
            "rp6502_map(${target} ${header}): ${header} is already mapped for ${target}.")
    endif()
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}.map/${id}")
    string(REPLACE "\\" "/" header_c "${header_file}")

    # A program that prints the values. Everything is unsigned long, so
    # neither compiler's 16 bit size_t truncates what it prints. Included by
    # name with -I below, because cc65 cannot find a quoted include given as
    # an absolute path.
    set(stub "#include <stdio.h>\n#include \"${header_name}\"\n\nint main(void)\n{\n")
    foreach(name line IN ZIP_LISTS names lines)
        string(APPEND stub
            "#ifdef ${name}\n"
            "#line ${line} \"${header_c}\"\n"
            "    printf(\"${name} 0x%lX\\n\", (unsigned long)${name});\n"
            "#endif\n")
    endforeach()
    string(APPEND stub "    return 0;\n}\n")
    file(WRITE "${dir}/map_stub.c" "${stub}")

    # A program of assertions, compiled but never run, so the compiler
    # reports a bad layout against the line in the header. Every name gets a
    # line of its own, since a name with no assertion would let a define
    # that will not compile through. Each declaration is one line, because
    # cc65 reports the line it finished reading rather than the one it
    # started on.
    set(check "#include <stddef.h>\n#include \"${header_name}\"\n\n")
    string(APPEND check
        "/* size_t is 16 bits, so a structure larger than 64K wraps instead of\n"
        "   being refused. An array of one is refused outright, because the\n"
        "   size of an array is checked against the largest object the target\n"
        "   allows and not against the truncated sizeof. */\n")
    # The leading underscore is the namespace C keeps for file scope, which
    # is the one place a name here cannot collide with the header's own.
    foreach(type line guard IN ZIP_LISTS probes probe_lines probe_guards)
        string(MAKE_C_IDENTIFIER "${type}" probe)
        string(APPEND check
            "\n#if ${guard}\n#line ${line} \"${header_c}\"\n"
            "extern ${type} _map_fits_${probe}[1];\n#endif\n")
    endforeach()
    # An offset and arithmetic between offsets are size_t, so 16 bits, and
    # can never trip this. A number that does not fit is a long and does,
    # and so does a negative one, which the cast makes large.
    foreach(name line IN ZIP_LISTS names lines)
        string(APPEND check
            "\n#ifdef ${name}\n#line ${line} \"${header_c}\"\n"
            "_Static_assert((unsigned long)(${name}) < 0x10000UL,"
            " \"${name} does not fit in 16 bits.\");\n")
        if (NOT unaligned OR NOT name MATCHES "^(${unaligned})$")
            string(APPEND check
                "#line ${line} \"${header_c}\"\n"
                "_Static_assert(!((${name}) & 1), \"${name} is unaligned."
                " To allow, use the [<unaligned_regex>] in rp6502_map.\");\n")
        endif()
        string(APPEND check "#endif\n")
    endforeach()
    file(WRITE "${dir}/map_check.c" "${check}")

    # cc65's CMAKE_C_COMPILER is a wrapper around cl65 that puts diagnostics
    # in the form an IDE matches, so both programs are built through it.
    set(compiler_args)
    if (CMAKE_C_COMPILER_ID STREQUAL "cc65")
        set(compiler_args -P "${RP6502_TOOLS_DIR}/cc65-toolchain.cmake" -- "${CC65_C_COMPILER}")
    elseif (CMAKE_C_COMPILER_ARG1)
        separate_arguments(compiler_args NATIVE_COMMAND "${CMAKE_C_COMPILER_ARG1}")
    endif()
    separate_arguments(flags NATIVE_COMMAND "${CMAKE_C_FLAGS}")
    # clang does not escape spaces in the -MT target, so the target is a
    # plain name.
    separate_arguments(dep_flags NATIVE_COMMAND "${CMAKE_DEPFILE_FLAGS_C}")
    string(REPLACE "<DEP_TARGET>" "map_stub" dep_flags "${dep_flags}")
    string(REPLACE "<DEP_FILE>" "${dir}/map_stub.d" dep_flags "${dep_flags}")

    set(failed FALSE)
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" ${compiler_args} ${flags} -I "${header_dir}"
                ${dep_flags} -o "${dir}/map_stub" "${dir}/map_stub.c"
        WORKING_DIRECTORY "${dir}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output
    )
    if (NOT result EQUAL 0)
        set(failed TRUE)
    endif()

    # The addresses are read at configure time, so a change to any header
    # the stub includes has to configure the project again, not only a
    # change to the named one. The list is read after a failed compile too,
    # so fixing an included header configures again. A failed cc65 compile
    # keeps the previous list, which can name a header that no longer exists.
    if (EXISTS "${dir}/map_stub.d")
        file(READ "${dir}/map_stub.d" deps)
        # Make syntax, where a name that ends in a colon is a target.
        string(REGEX REPLACE "\\\\\r?\n" " " deps "${deps}")
        string(REGEX REPLACE "([^\\\\])[ \t\r\n]+" "\\1;" deps "${deps}")
        foreach(dep IN LISTS deps)
            if (dep STREQUAL "" OR dep MATCHES ":$")
                continue()
            endif()
            string(REPLACE "\\ " " " dep "${dep}")
            string(REPLACE "\\#" "#" dep "${dep}")
            string(REPLACE "$$" "$" dep "${dep}")
            cmake_path(ABSOLUTE_PATH dep BASE_DIRECTORY "${dir}" NORMALIZE)
            if (EXISTS "${dep}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${dep}")
            endif()
        endforeach()
    endif()

    if (NOT failed)
        rp6502_default_address(load_addr)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${RP6502_TOOLS_DIR}/rp6502.py"
                    -a "${load_addr}" -r "${load_addr}"
                    -o "${dir}/map_stub.rp6502" create "${dir}/map_stub"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE output
        )
        if (NOT result EQUAL 0)
            set(failed TRUE)
        endif()
    endif()

    if (NOT failed)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${RP6502_TOOLS_DIR}/rp6502.py"
                    -c "${RP6502_PROJECT_DIR}/.rp6502"
                    execute "${dir}/map_stub.rp6502"
            TIMEOUT 60
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE output
        )
        if (NOT result EQUAL 0)
            set(failed TRUE)
        endif()
    endif()

    # A failure here is the header's, and the build reports it in full, so
    # the configure finishes with every address unread rather than leaving
    # the project unconfigured. Unread is 0xFFFFFFFF, which RAM() and
    # XRAM() refuse and rp6502.py cannot place, so no ROM is built from it.
    string(REPLACE "\r" "" output "${output}")
    foreach(name IN LISTS names)
        set(found "")
        if (failed)
            set(found 0xFFFFFFFF)
        elseif (output MATCHES "(^|\n)${name} (0x[0-9A-Fa-f]+)")
            # Out of range is unread while the build reports it against the
            # header.
            set(found "${CMAKE_MATCH_2}")
            math(EXPR numeric "${found}")
            if (numeric GREATER 65535)
                set(found 0xFFFFFFFF)
            endif()
        endif()
        # A name the preprocessor skipped is not a name at all.
        if (found STREQUAL "")
            continue()
        endif()
        # One header may define a name twice under #if, but two maps of one
        # target never share a name. Names from a header that was not read
        # are not recorded, since some of them may sit in a false #if.
        if (NOT failed)
            get_target_property(from ${target} RP6502_MAP_FROM_${name})
            if (NOT from MATCHES "-NOTFOUND$" AND NOT from STREQUAL header_file)
                file(RELATIVE_PATH from "${CMAKE_CURRENT_SOURCE_DIR}" "${from}")
                message(FATAL_ERROR
                    "rp6502_map(${target} ${header}): ${name} is already defined by "
                    "rp6502_map(${target} ${from}).")
            endif()
            set_property(TARGET ${target} PROPERTY RP6502_MAP_FROM_${name} "${header_file}")
        endif()
        set_property(TARGET ${target} PROPERTY RP6502_MAP_NAME_${name} "${found}")
    endforeach()
    # A header that will not compile is reported by the compile below, but a
    # tool that did not run leaves a header that compiles and nothing to
    # report. So the reason is carried to the build and fails it.
    set(unread)
    if (failed)
        message(STATUS "rp6502_map(${target} ${header}) read no addresses; the build reports why.")
        file(WRITE "${dir}/map_unread.txt"
            "rp6502_map(${target} ${header}) read no addresses. Configure again once"
            " this is fixed.\n${output}\n")
        set(unread
            COMMAND "${CMAKE_COMMAND}" -E cat "${dir}/map_unread.txt"
            COMMAND "${CMAKE_COMMAND}" -E false)
    endif()

    set(map_check "${target}_map_${id}")
    add_custom_command(
        OUTPUT "${dir}/map_check.stamp"
        DEPENDS "${header_file}" "${dir}/map_check.c"
        COMMAND "${CMAKE_C_COMPILER}" ${compiler_args} ${flags} -I "${header_dir}"
                -c -o "${dir}/map_check.o" "${dir}/map_check.c"
        ${unread}
        COMMAND "${CMAKE_COMMAND}" -E touch "${dir}/map_check.stamp"
        COMMENT "Checking ${header_name}"
        VERBATIM
    )
    add_custom_target(${map_check} ALL DEPENDS "${dir}/map_check.stamp")
    set_property(TARGET ${target} APPEND PROPERTY RP6502_MAP_CHECKS "${map_check}")
endfunction()

# Declare files as byproducts of building <target>.
#
# RP6502 Byproducts
# ^^^^^^^^^^^^^^^^^
#
#  rp6502_byproducts(<target> <file>...)
#
# Some linker configurations write extra outputs alongside the main
# executable. CMake's add_executable() does not model these.
#
function(rp6502_byproducts target)
    add_custom_command(
        OUTPUT ${ARGN}
        DEPENDS ${target}
        COMMAND ${CMAKE_COMMAND} -E touch_nocreate ${ARGN}
        VERBATIM
    )
endfunction()
