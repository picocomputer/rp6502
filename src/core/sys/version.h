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

    /* "Version 0.31", "CI <run id>", or the time it was compiled. */
    const char *version_string(void);

    /* "0.31" where the above says "Version 0.31". */
    const char *version_bare(void);

#ifdef __cplusplus
}
#endif

#endif /* _CORE_SYS_VERSION_H_ */
