/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Resolving a path the way the drive under it would: the CWD a relative path
 * is relative to, and the case a name is really stored in, are both questions
 * only FatFs can answer, and this machine is the one with a FatFs.
 */

#include "sys/path.h"
#include "core/str/oem.h"
#include "core/str/path.h"
#include "core/str/str.h"
#include "fatfs/ff.h"
#include <ctype.h>
#include <string.h>

/* Its own scratch, so a caller may hand it a string it parsed and still not
 * have to think about whose buffer that was. The result is valid until the
 * next call. */
static char path_buf[256];

const char *path_abs(const char *path)
{
    size_t drive_len;
    const char *segs_src;
    const char *colon = strchr(path, ':');
    // A drive-prefixed path without a separator after the colon is
    // relative to that drive's current directory. ":name" (installed
    // ROM namespace) stays absolute.
    bool relative = colon ? (colon != path && !path_is_sep(colon[1]))
                          : !path_is_sep(path[0]);

    if (colon && !relative)
    {
        drive_len = (size_t)(colon - path) + 1;
        if (drive_len >= sizeof(path_buf))
            return NULL;
        for (size_t i = 0; i + 1 < drive_len; i++)
            path_buf[i] = (char)toupper((unsigned char)path[i]);
        path_buf[drive_len - 1] = ':';
        segs_src = colon + 1;
        if (path_is_sep(*segs_src))
            segs_src++;
    }
    else
    {
        if (f_getcwd(path_buf, sizeof(path_buf)) != FR_OK)
            return NULL;
        if (colon)
        {
            // f_getcwd only reads the current drive: save it, hop to
            // the target drive for its current directory, and hop back.
            const char *save_colon = strchr(path_buf, ':');
            if (!save_colon)
                return NULL;
            char save[6];
            size_t save_len = (size_t)(save_colon + 1 - path_buf);
            memcpy(save, path_buf, save_len);
            save[save_len] = '\0';
            if (f_chdrive(path) != FR_OK)
                return NULL;
            FRESULT fr = f_getcwd(path_buf, sizeof(path_buf));
            f_chdrive(save);
            if (fr != FR_OK)
                return NULL;
        }
        const char *buf_colon = strchr(path_buf, ':');
        if (!buf_colon)
            return NULL;
        drive_len = (size_t)(buf_colon - path_buf) + 1;
        segs_src = colon ? colon + 1 : path;
        if (path_is_sep(*segs_src))
            segs_src++;
    }

    // For relative paths start at the end of the CWD already in path_buf.
    // For absolute paths start fresh after the drive prefix.
    size_t out;
    if (relative)
    {
        out = strlen(path_buf);
        while (out > drive_len && path_buf[out - 1] == '/')
            out--;
    }
    else
    {
        out = drive_len;
    }

    // Write segments into path_buf, resolve . and ..
    const char *seg = segs_src;
    while (*seg)
    {
        const char *next = seg;
        while (*next && !path_is_sep(*next))
            next++;
        size_t slen = (size_t)(next - seg);
        if (!*next)
            next = NULL;
        if (slen == 0 || (slen == 1 && seg[0] == '.'))
        {
            // skip empty segment or "."
        }
        else if (slen == 2 && seg[0] == '.' && seg[1] == '.')
        {
            if (out > drive_len)
            {
                out--;
                while (out > drive_len && path_buf[out] != '/')
                    out--;
            }
        }
        else
        {
            if (out + 1 + slen > 255)
                return NULL;
            if (drive_len == 1) // ":" installed ROM
                for (size_t k = 0; k < slen; k++)
                    path_buf[out++] = (char)toupper((unsigned char)seg[k]);
            else
            {
                path_buf[out++] = '/';
                memcpy(path_buf + out, seg, slen);
                out += slen;
            }
        }
        seg = next ? next + 1 : seg + slen;
    }

    if (out == drive_len)
        path_buf[out++] = '/';
    path_buf[out] = '\0';
    return path_buf;
}

bool path_correct_basename(char *path, size_t path_size)
{
    char fname[FF_LFN_BUF + 1];
    if (!path_lookup_basename(path, fname, sizeof fname))
        return true; // lookup failed; leave the input case unchanged

    // Find where the basename starts (after the last separator or drive
    // colon), ignoring any trailing separators on path.
    size_t end = strlen(path);
    while (end > 0 && path_is_sep(path[end - 1]))
        end--;

    size_t name_start = 0;
    const char *colon = strchr(path, ':');
    if (colon && (size_t)(colon - path) < end)
        name_start = (size_t)(colon - path) + 1;
    for (size_t i = name_start; i < end; i++)
        if (path_is_sep(path[i]))
            name_start = i + 1;

    size_t fname_len = strlen(fname);
    if (name_start + fname_len + 1 > path_size)
        return false;
    memcpy(path + name_start, fname, fname_len + 1);
    return true;
}

bool path_lookup_basename(const char *path, char *out, size_t out_size)
{
    if (out_size)
        out[0] = '\0';

    // Trim trailing separators so "folder/" is treated like "folder".
    size_t end = strlen(path);
    while (end > 0 && path_is_sep(path[end - 1]))
        end--;
    if (end == 0)
        return false;

    // Find basename. ':' caps the drive prefix; treat the byte after it
    // as the start of the path so "X:foo" splits as parent "X:" + name "foo".
    size_t name_start = 0;
    const char *colon = strchr(path, ':');
    if (colon)
        name_start = (size_t)(colon - path) + 1;
    for (size_t i = name_start; i < end; i++)
        if (path_is_sep(path[i]))
            name_start = i + 1;

    size_t name_len = end - name_start;
    if (name_len == 0 || name_len > FF_LFN_BUF)
        return false;

    char name[FF_LFN_BUF + 1];
    memcpy(name, path + name_start, name_len);
    name[name_len] = '\0';

    // Build parent. Drop a trailing '/' or '\' unless we'd be left with
    // a bare drive prefix ("X:") or the root separator alone ("/"), in
    // which case f_opendir needs the separator kept.
    char parent[FF_LFN_BUF + 1];
    if (name_start == 0)
    {
        parent[0] = '.';
        parent[1] = '\0';
    }
    else
    {
        size_t parent_len = name_start;
        if (parent_len > 1 && path_is_sep(path[parent_len - 1]) &&
            path[parent_len - 2] != ':')
            parent_len--;
        if (parent_len >= sizeof parent)
            return false;
        memcpy(parent, path, parent_len);
        parent[parent_len] = '\0';
    }

    DIR dir;
    if (f_opendir(&dir, parent) != FR_OK)
        return false;
    bool found = false;
    FILINFO fno;
    for (;;)
    {
        if (f_readdir(&dir, &fno) != FR_OK || !fno.fname[0])
            break;
        if (str_oem_eq(fno.fname, name))
        {
            size_t flen = strlen(fno.fname);
            if (flen + 1 <= out_size)
            {
                memcpy(out, fno.fname, flen + 1);
                found = true;
            }
            break;
        }
    }
    f_closedir(&dir);
    return found;
}
