/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The Pocket's process glue, sw/proc.c, with the core's process rules,
 * core/api/proc.c, and stand-ins for the card, the ROM loader, the machine
 * and the console. A chain's end is counted by the console's clear of its
 * type-ahead, which proc_chain_end makes once.
 */

#include "host/pocket/sw/proc.h"
#include "host/pocket/sw/fs.h"
#include "host/pocket/sw/rom.h"
#include "core/api/arg.h"
#include "core/com/com.h"
#include "core/com/tio.h"
#include "core/str/oem.h"
#include "core/sys/sys.h"

#include "utest.h"

#include <stdlib.h>
#include <string.h>

static int pproc_chain_ends;

void com_rx_clear(void) { pproc_chain_ends++; }
void com_end_reset(void) {}
void com_flush(void) {}
void com_set_bel(bool on) { (void)on; }
void oem_restore_code_page(void) {}
void tio_cursor_default(void) {}

void sys_stop(void) {}
void sys_break_request(void) {}
bool sys_active(void) { return false; }

char *os_dir_path_hold(const char *path) { return strdup(path); }
void os_dir_path_drop(char *path) { free(path); }

bool fs_getfile(uint32_t slot, char *out, size_t cap)
{
    (void)slot;
    (void)out;
    (void)cap;
    return false;
}

const char *fs_strip_drive(const char *path) { return path; }

bool fs_card_path(const char *path, const char *root, char *out, size_t cap)
{
    (void)root;
    if (strlen(path) >= cap)
        return false;
    strcpy(out, path);
    return true;
}

bool rom_load(const char *path)
{
    (void)path;
    return true;
}

static void pproc_start(const char *path)
{
    arg_clear();
    arg_append(path);
    proc_run();
}

/* A launcher has started a child, and no chain end is counted yet. The
 * restage first drops whatever the test before left. */
static void pproc_launcher_and_child(void)
{
    proc_cancel_launcher();
    proc_restage();
    pproc_start("FS:/launcher.rp6502");
    proc_set_launcher(true);
    pproc_start("FS:/child.rp6502");
    pproc_chain_ends = 0;
}

/* A start by name from the Pocket's menu: the launcher goes before the stop,
 * the stop fans out, and the menu's ROM is staged. */
static void pproc_menu_load(void)
{
    proc_cancel_launcher();
    proc_stop();
    proc_restage();
}

UTEST(pproc, a_menu_load_ends_the_chain_once)
{
    pproc_launcher_and_child();
    pproc_menu_load();
    ASSERT_EQ(1, pproc_chain_ends);
    ASSERT_FALSE(proc_has_launcher());
    ASSERT_FALSE(proc_exec_take());
}

UTEST(pproc, a_menu_load_ends_the_chain_an_exec_left_open)
{
    pproc_launcher_and_child();
    arg_clear();
    arg_append("FS:/next.rp6502");
    proc_exec_start();
    pproc_menu_load();
    ASSERT_EQ(1, pproc_chain_ends);
    ASSERT_FALSE(proc_has_launcher());
    ASSERT_FALSE(proc_exec_take());
}

UTEST(pproc, an_exec_keeps_the_chain)
{
    pproc_launcher_and_child();
    arg_clear();
    arg_append("FS:/next.rp6502");
    proc_exec_start();
    proc_stop();
    ASSERT_TRUE(proc_exec_take());
    ASSERT_EQ(0, pproc_chain_ends);
    ASSERT_TRUE(proc_has_launcher());
}

UTEST_MAIN();
