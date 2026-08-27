/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's drive, as core/api/dir.h asks for it -- which is almost
 * nothing. There are no directories on an APF data slot: a program opens a
 * name and the host binds a slot to it, so fifteen of the seventeen slots
 * are left NULL and core/api/dir.c answers ENOSYS for them. The two that are
 * filled are the two that can be answered without a directory at all.
 *
 * The files themselves are next door in fs.c, which owns the slot pool and
 * the bridge this shares a drive letter with.
 */

#include "fs.h"

#include "core/api/dir.h"

/* ---- The drive, as core/api/dir.c asks for it ---------------------------- */

/* Synthetic: the host cannot be asked. Spelled from the drive so
 * appending a name opens the same file the bare name does. */
static bool drive_getcwd(char *buf, size_t size, api_errno *err)
{
    static const char cwd[] = "MSC0:/Saves/rp6502/common/";
    if (size < sizeof cwd)
    {
        *err = API_ENOMEM;
        return false;
    }
    memcpy(buf, cwd, sizeof cwd);
    return true;
}

static bool drive_chdrive(const char *drive, api_errno *err)
{
    const char *rest = fs_strip_drive(drive);
    if (!rest || *rest)
    {
        *err = API_ENODEV;
        return false;
    }
    return true;
}

/* One folder, no directories to walk and no metadata to read, so all this
 * drive answers is where it is and that it is the only one. The rest of the
 * slots stay empty and the syscalls above them say ENOSYS. */
const dir_backend_t drive_backend = {
    .chdrive = drive_chdrive,
    .getcwd = drive_getcwd,
};
