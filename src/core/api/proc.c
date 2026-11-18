/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See proc.h.
 */

#include "core/api/proc.h"
#include "core/api/api.h"
#include "core/api/arg.h"
#include "core/str/oem.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/hid/usage.h"
#include "core/sys/sys.h"
#include "machine.h"
#include "osal/dir.h"

#include <string.h>

static char *proc_running_path;
static char *proc_launcher_path;
static int16_t proc_exit_code;

static void HOST_IN_RAM_FUNC set_path(char **slot, const char *path)
{
    if (*slot)
        os_dir_path_drop(*slot);
    *slot = path && path[0] ? os_dir_path_hold(path) : NULL;
}

void proc_run(void)
{
    set_path(&proc_running_path, arg_index(0));
}

const char *proc_running(void)
{
    return proc_running_path ? proc_running_path : "";
}

bool proc_has_launcher(void)
{
    return proc_launcher_path != NULL;
}

const char *proc_launcher(void)
{
    return proc_launcher_path ? proc_launcher_path : "";
}

void proc_chain_sst_save(sst_cursor_t *c)
{
    sst_put_i16(c, proc_exit_code);
    sst_put_str(c, proc_running(), PROC_PATH_SLOT);
    sst_put_str(c, proc_launcher(), PROC_PATH_SLOT);
    sst_put(c, arg_data(), arg_bytes());
}

/* The paths are put back here rather than through proc_run, because
 * proc_run takes argv[0] from the argument buffer. */
bool proc_chain_sst_load(sst_cursor_t *c)
{
    int16_t code = sst_get_i16(c);
    const char *running = sst_view_str(c, PROC_PATH_SLOT);
    const char *launcher = sst_view_str(c, PROC_PATH_SLOT);
    const uint8_t *argv = sst_view(c, arg_bytes());
    if (!sst_ok(c))
        return false;
    proc_exit_code = code;
    set_path(&proc_running_path, running);
    set_path(&proc_launcher_path, launcher);
    arg_set_data(argv);
    return true;
}

void proc_set_launcher(bool is_launcher)
{
    set_path(&proc_launcher_path, is_launcher ? proc_running_path : NULL);
}

void HOST_IN_RAM_FUNC proc_cancel_launcher(void)
{
    set_path(&proc_launcher_path, NULL);
}

bool proc_is_launcher(void)
{
    return proc_launcher_path && proc_running_path &&
           strcmp(proc_running_path, proc_launcher_path) == 0;
}

int16_t proc_get_exit_code(void)
{
    return proc_exit_code;
}

void HOST_IN_RAM_FUNC proc_set_exit_code(int16_t code)
{
    proc_exit_code = code;
}

void HOST_IN_RAM_FUNC proc_exit(int16_t exit_code)
{
    proc_set_exit_code(exit_code);
    sys_stop();
}

void HOST_IN_RAM_FUNC proc_break(void)
{
    proc_cancel_launcher();
    sys_break_request();
}

/* Ctrl-Alt-Delete. False on a machine with no monitor, where the key types. */
static bool proc_break_to_monitor(void)
{
    if (!RP6502_HAS_MONITOR)
        return false;
    proc_break();
    return true;
}

/* Alt-F4: the program exits as exit(-1) does. False when nothing runs, from the
 * launcher itself, and with no launcher on a machine with no monitor, where the
 * key types. */
static bool proc_quit(void)
{
    if (!sys_running() || proc_is_launcher() ||
        (!RP6502_HAS_MONITOR && !proc_has_launcher()))
        return false;
    proc_exit(-1);
    return true;
}

bool HOST_IN_RAM_FUNC proc_key_chord(uint8_t keycode, bool ctrl, bool alt)
{
    if (keycode == HID_KEY_F4 && alt)
        return proc_quit();
    if ((keycode == HID_KEY_DELETE || keycode == HID_KEY_KEYPAD_DECIMAL) && ctrl && alt)
        return proc_break_to_monitor();
    return false;
}

void proc_stop(void)
{
    /* A load the exiting program or the user already asked for is on its
     * way, so the launcher must not be scheduled over it. */
    bool inflight = proc_exec_inflight();
    bool relaunch = !inflight && !proc_is_launcher() && proc_has_launcher();
    set_path(&proc_running_path, NULL);
    if (inflight)
        return;
    if (relaunch)
    {
        arg_clear();
        arg_append(proc_launcher_path);
        proc_exec_relaunch();
        if (proc_exec_inflight())
            return;
    }
    proc_chain_end();
}

void proc_chain_end(void)
{
    proc_cancel_launcher();
    com_rx_clear();
    com_end_reset();
    /* Output reaches the VGA before a code page change clears it. */
    manifold_flush();
    oem_restore_code_page();
    com_set_bel(true);
}

void proc_exec_failed(void)
{
    proc_set_exit_code(1);
    proc_cancel_launcher();
    /* Nothing else ends a chain whose load failed after the stop. */
    if (!sys_active())
        proc_chain_end();
}

bool HOST_IN_FLASH_FUNC proc_api_argv(void)
{
    if (xstack_ptr != XSTACK_SIZE)
        return api_return_errno(API_EINVAL);
    return api_return_ax(arg_push_xstack());
}

/* op 0x09. The xstack holds the new argv, whose argv[0] is the path of the
 * .rp6502. The op succeeds as soon as the argv parses and argv[0] fits in a
 * path, because by the time a load can fail, the program that called EXEC has
 * already stopped; the error is printed on the console. A refused argv leaves
 * the argv empty, as a layout that does not parse does. */
bool HOST_IN_FLASH_FUNC proc_api_exec(void)
{
    if (!arg_pull_xstack(API_PATH_MAX))
        return api_return_errno(API_EINVAL);
    proc_exec_start();
    return api_return_ax(0);
}
