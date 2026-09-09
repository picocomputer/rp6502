/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_SYS_VERSION_H_
#define _CORE_SYS_VERSION_H_

#ifdef __cplusplus
extern "C"
{
#endif

    /* "Version 0.31" for a tagged build, "CI <run id>" for an untagged CI
     * build, else a build timestamp. */
    const char *version_string(void);

    /* The same, without the "Version " a caller's own UI supplies. */
    const char *version_bare(void);

#ifdef __cplusplus
}
#endif

#endif /* _CORE_SYS_VERSION_H_ */
