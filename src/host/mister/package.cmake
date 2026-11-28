# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# cmake -DBUILD=<q17 build> -DTREE=<exported tree> -DPKG=<folder>
#       -DZIP=<file> -P package.cmake
#
# Lays out the release the way a MiSTer's SD card holds it, under PKG, and
# zips it. The rbf is dated as MiSTer's releases are, RP6502_yyyymmdd.rbf,
# by the BUILD_DATE that sys/build_id.tcl wrote for the fit, so the date
# is the one the OSD's V line shows. The rbf also goes into the tree's
# releases/, which the export empties.

file(READ ${BUILD}/build_id.v id)
if(NOT id MATCHES "BUILD_DATE \"([0-9][0-9][0-9][0-9][0-9][0-9])\"")
    message(FATAL_ERROR "${BUILD}/build_id.v has no BUILD_DATE")
endif()
set(rbf RP6502_20${CMAKE_MATCH_1}.rbf)

file(REMOVE_RECURSE ${PKG} ${ZIP})
file(MAKE_DIRECTORY ${PKG}/_Computer ${TREE}/releases)
file(COPY_FILE ${BUILD}/output_files/RP6502.rbf ${PKG}/_Computer/${rbf})
file(COPY_FILE ${BUILD}/output_files/RP6502.rbf ${TREE}/releases/${rbf})
execute_process(COMMAND ${CMAKE_COMMAND} -E tar cf ${ZIP} --format=zip
        _Computer
    WORKING_DIRECTORY ${PKG}
    RESULT_VARIABLE rc)
if(rc)
    message(FATAL_ERROR "zipping ${PKG} failed")
endif()
