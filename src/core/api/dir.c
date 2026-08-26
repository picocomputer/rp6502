/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See dir.h.
 */

#include "core/api/dir.h"
#include "core/main.h"

#include <string.h>

/* How many entries each open directory has handed out. telldir reports it,
 * seekdir winds to it, rewinddir zeroes it -- and no drive needs to know,
 * because a directory read is a directory read. */
static int32_t tells[DIR_MAX_OPEN];

static bool dir_push_filinfo(FILINFO *fno)
{
    /* Push fields in reverse so they land in forward
     * order in the 6502-visible struct. */
    bool ok = true;
    for (int i = FF_LFN_BUF; i >= 0; i--)
        ok &= api_push_char(&fno->fname[i]);
    for (int i = FF_SFN_BUF; i >= 0; i--)
        ok &= api_push_char(&fno->altname[i]);
    ok &= api_push_uint8(&fno->fattrib);
    ok &= api_push_uint16(&fno->crtime);
    ok &= api_push_uint16(&fno->crdate);
    ok &= api_push_uint16(&fno->ftime);
    ok &= api_push_uint16(&fno->fdate);
    uint32_t fsize = fno->fsize;
    if (fno->fsize > 0xFFFFFFFF)
        fsize = 0xFFFFFFFF;
    ok &= api_push_uint32(&fsize);
    return ok;
}

/* The path a whole-path op was called with, consuming the xstack. */
static const char *dir_path(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    return path;
}

/* What a backend said, as the 6502 sees it. */
static bool dir_return(bool ok, api_errno err)
{
    return ok ? api_return_ax(0) : api_return_errno(err);
}

/* A call this drive does not have. The xstack is left where it is, which is
 * what an op with no handler at all does. */
static bool dir_enosys(void)
{
    return api_return_errno(API_ENOSYS);
}

/* Read one entry through the backend and count it. End-of-directory is an
 * empty name, not an error, and does not advance the counter. */
static bool dir_next(const dir_backend_t *d, int des, FILINFO *fno, api_errno *err)
{
    if (!d->readdir(des, fno, err))
        return false;
    if (fno->fname[0])
        tells[des]++;
    return true;
}

/* Nothing stays open across a machine event: a run starts with none, and a
 * stop leaves none behind. */
void dir_run(void)
{
    dir_stop();
    for (int i = 0; i < DIR_MAX_OPEN; i++)
        tells[i] = 0;
}

void dir_stop(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->closedir)
        return;
    api_errno err;
    for (int i = 0; i < DIR_MAX_OPEN; i++)
        if (d->validate(i, &err))
            d->closedir(i, &err);
}

bool dir_api_stat(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->stat)
        return dir_enosys();
    FILINFO fno;
    api_errno err;
    if (!d->stat(dir_path(), &fno, &err))
        return api_return_errno(err);
    if (!dir_push_filinfo(&fno))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

bool dir_api_opendir(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->opendir)
        return dir_enosys();
    int des;
    api_errno err;
    if (!d->opendir(dir_path(), &des, &err))
        return api_return_errno(err);
    tells[des] = 0;
    return api_return_ax((uint16_t)des);
}

bool dir_api_readdir(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->readdir)
        return dir_enosys();
    int des = API_A;
    api_errno err;
    if (!d->validate(des, &err))
        return api_return_errno(err);
    FILINFO fno;
    if (!dir_next(d, des, &fno, &err))
        return api_return_errno(err);
    if (!dir_push_filinfo(&fno))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

bool dir_api_closedir(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->closedir)
        return dir_enosys();
    int des = API_A;
    api_errno err;
    if (!d->validate(des, &err))
        return api_return_errno(err);
    return dir_return(d->closedir(des, &err), err);
}

bool dir_api_telldir(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->readdir)
        return dir_enosys();
    int des = API_A;
    api_errno err;
    if (!d->validate(des, &err))
        return api_return_errno(err);
    return api_return_axsreg((uint32_t)tells[des]);
}

/* Seek by entry index: wind back to the start if the target is behind, then
 * read forward to it. Past the end is EINVAL -- there is no such entry to be
 * positioned at. */
bool dir_api_seekdir(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->readdir || !d->rewinddir)
        return dir_enosys();
    int des = API_A;
    int32_t offs;
    if (!api_pop_int32_end(&offs))
        return api_return_errno(API_EINVAL);
    api_errno err;
    if (!d->validate(des, &err))
        return api_return_errno(err);
    if (offs < 0)
        return api_return_errno(API_EINVAL);
    if (tells[des] > offs)
    {
        if (!d->rewinddir(des, &err))
            return api_return_errno(err);
        tells[des] = 0;
    }
    while (tells[des] < offs)
    {
        FILINFO fno;
        if (!dir_next(d, des, &fno, &err))
            return api_return_errno(err);
        if (!fno.fname[0])
            return api_return_errno(API_EINVAL);
    }
    return api_return_ax(0);
}

bool dir_api_rewinddir(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->rewinddir)
        return dir_enosys();
    int des = API_A;
    api_errno err;
    if (!d->validate(des, &err))
        return api_return_errno(err);
    if (!d->rewinddir(des, &err))
        return api_return_errno(err);
    tells[des] = 0;
    return api_return_ax(0);
}

bool dir_api_unlink(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->unlink)
        return dir_enosys();
    api_errno err;
    return dir_return(d->unlink(dir_path(), &err), err);
}

bool dir_api_rename(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->rename)
        return dir_enosys();
    /* The xstack holds newname\0oldname; rename takes them the other way. */
    const char *newname = dir_path();
    const char *oldname = newname;
    while (*oldname)
        oldname++;
    if (oldname == (const char *)&xstack[XSTACK_SIZE])
        return api_return_errno(API_EINVAL);
    oldname++;
    api_errno err;
    return dir_return(d->rename(oldname, newname, &err), err);
}

bool dir_api_chmod(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->chmod)
        return dir_enosys();
    uint8_t mask = API_A;
    uint8_t attr;
    if (!api_pop_uint8(&attr))
        return api_return_errno(API_EINVAL);
    api_errno err;
    return dir_return(d->chmod(dir_path(), attr, mask, &err), err);
}

bool dir_api_utime(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->utime)
        return dir_enosys();
    /* All four are popped whether or not this drive stores creation times,
     * because the 6502 pushed all four. */
    FILINFO fno;
    fno.crtime = API_AX;
    if (!api_pop_uint16(&fno.crdate) ||
        !api_pop_uint16(&fno.ftime) ||
        !api_pop_uint16(&fno.fdate))
        return api_return_errno(API_EINVAL);
    api_errno err;
    return dir_return(d->utime(dir_path(), &fno, &err), err);
}

bool dir_api_mkdir(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->mkdir)
        return dir_enosys();
    api_errno err;
    return dir_return(d->mkdir(dir_path(), &err), err);
}

bool dir_api_chdir(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->chdir)
        return dir_enosys();
    api_errno err;
    return dir_return(d->chdir(dir_path(), &err), err);
}

bool dir_api_chdrive(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->chdrive)
        return dir_enosys();
    api_errno err;
    return dir_return(d->chdrive(dir_path(), &err), err);
}

/* The drive writes the path at the bottom of the xstack; it is relocated to
 * the top so the 6502 pops it in order. */
bool dir_api_getcwd(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->getcwd)
        return dir_enosys();
    api_errno err;
    bool ok = d->getcwd((char *)xstack, XSTACK_SIZE, &err);
    xstack_ptr = XSTACK_SIZE;
    if (!ok)
        return api_return_errno(err);
    uint16_t len = (uint16_t)strlen((char *)xstack);
    for (uint16_t i = len; i;)
        xstack[--xstack_ptr] = xstack[--i];
    return api_return_ax(len + 1);
}

bool dir_api_setlabel(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->setlabel)
        return dir_enosys();
    api_errno err;
    return dir_return(d->setlabel(dir_path(), &err), err);
}

bool dir_api_getlabel(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->getlabel)
        return dir_enosys();
    char label[12];
    api_errno err;
    if (!d->getlabel(dir_path(), label, sizeof(label), &err))
        return api_return_errno(err);
    size_t len = strlen(label);
    for (size_t i = len; i;)
        if (!api_push_char(&label[--i]))
            return api_return_errno(API_ENOMEM);
    return api_return_ax((uint16_t)(len + 1));
}

bool dir_api_getfree(void)
{
    const dir_backend_t *d = main_dir_backend();
    if (!d->getfree)
        return dir_enosys();
    uint32_t tot_sect, fre_sect;
    api_errno err;
    if (!d->getfree(dir_path(), &tot_sect, &fre_sect, &err))
        return api_return_errno(err);
    if (!api_push_uint32(&tot_sect) || !api_push_uint32(&fre_sect))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}
