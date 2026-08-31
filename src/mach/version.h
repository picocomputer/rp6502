/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _MACH_VERSION_H_
#define _MACH_VERSION_H_

#ifdef __cplusplus
extern "C"
{
#endif

    /* What this build is: "Version 0.31", "CI <run id>", or the time it was
     * compiled. The same string the RIA and VGA firmware print in their
     * banners, from the same generated header. */
    const char *version_string(void);

    /* The same, without the word a frontend supplies itself -- "0.31" where
     * the above says "Version 0.31". Only a tagged build differs; a CI id and
     * a timestamp read the same either way. Decided by version.cmake's stamp
     * script, not by stripping a prefix off the string above. */
    const char *version_bare(void);

#ifdef __cplusplus
}
#endif

#endif /* _MACH_VERSION_H_ */
