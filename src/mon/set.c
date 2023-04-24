/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "set.h"
#include "cmd.h"
#include "str.h"
#include <stdio.h>

typedef void (*set_function)(const char *, size_t);
static struct
{
    size_t attr_len;
    const char *const attr;
    set_function func;
} const SETTERS[] = {
    {4, "caps", cmd_caps},
    {4, "phi2", cmd_phi2},
    {4, "resb", cmd_resb},
    {4, "boot", cmd_boot},
};
static const size_t SETTERS_COUNT = sizeof SETTERS / sizeof *SETTERS;

void set_attr(const char *args, size_t len)
{
    size_t i = 0;
    for (; i < len; i++)
        if (args[i] == ' ')
            break;
    size_t attr_len = i;
    for (; i < len; i++)
        if (args[i] != ' ')
            break;
    size_t args_start = i;
    for (i = 0; i < SETTERS_COUNT; i++)
    {
        if (attr_len == SETTERS[i].attr_len &&
            !strnicmp(args, SETTERS[i].attr, attr_len))
        {
            SETTERS[i].func(&args[args_start], len - args_start);
            return;
        }
    }
    printf("?Unknown attribute\n");
}
