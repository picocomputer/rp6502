/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _ATC_H_
#define _ATC_H_

void doAtCmds(char *atCmd);
char *factoryDefaults(char *atCmd);
char *resetToNvram(char *atCmd);

#endif /* _ATC_H_ */
