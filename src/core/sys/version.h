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

    /* What this build is: "Version 0.31", "CI <run id>", or the time it was
     * compiled. The same string the RIA and VGA firmware print in their
     * banners, from the same generated header. */
    const char *version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* _CORE_SYS_VERSION_H_ */
