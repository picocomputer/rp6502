/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vip.h"
#include "pico/rand.h"
#include <stdio.h>
#include <string.h>

void vip_print(void)
{
    char *patrons[] = {
        "Shawn Hyam",
        "Romain Fontaine",
        "Mark Rowe",
        "Vitali Filinkou",
        "Andy Herron",
        "Sean Franklin",
        "ulften",
    };
    const unsigned PATRONS_COUNT = sizeof(patrons) / sizeof(char *);
    for (unsigned i = 0; i < PATRONS_COUNT; i++)
    {
        unsigned swap = (PATRONS_COUNT * (get_rand_32() & 0xFFFF)) >> 16;
        char *tmp = patrons[i];
        patrons[i] = patrons[swap];
        patrons[swap] = tmp;
    }

    printf("          Patrons - %s", patrons[0]);
    unsigned col = 20 + strlen(patrons[0]) + 2;
    for (unsigned i = 1; i < PATRONS_COUNT - 1; i++)
    {
        printf(", ");
        col += strlen(patrons[i]) + 2;
        if (col > 78)
        {
            col = 20 + strlen(patrons[i]) + 2;
            printf("\n%20s", "");
        }
        printf("%s", patrons[i]);
    }
    puts(".");
}
