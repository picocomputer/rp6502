/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon/vip.h"
#include "pico/rand.h"
#include <stdio.h>
#include <string.h>

void vip_print(void)
{
    char *vips[] = {
        // Patreon
        "Shawn Hyam",
        "Romain Fontaine",
        "bdash",
        "Vitali Filinkou",
        "Andy Herron",
        "Sean Franklin",
        "ulften",
        "Larryvc",
        "ingmar meins",
        "Alexander Sharikhin",
        "Tom Smith",
        "michael sarr",
        "Kai Wells",
        "Andy Petrie",
        "Paul Gorlinsky",
        "Christian Lott",
        "Everett Rubel",
        "Cole Rise",
        "Randy Gardner",
        "Etienne Moreau",
        "EJ012345",
        "Ronald Lens",
        "Geoff Waldron",
        "Snake",
        "Kirk Davis",
        "Tomasz Sterna",
        "Brian E-RAD Simmons",
        "Robert Brown",
        "Andrew C. Young",
        "Jack Chidley",
        "tonyvr",
        "Jos Vermoesen",
        "James Temple",
        "Wojciech Gwiozdzik",
        "Volodymyr Vialyi",
        "markbo",
        "James Will",
        "David Raulo",
        "Sodiumlightbaby",
        "Paul S. Jenkins",
        "Muhammad A",
        "Ville Kivivuori",
        "Kamil Devel",
        "Jason Howard",
        "Bart DeMeulmeester",
        "Francis Cunningham",
        // YouTube
        "AJ_Whitney",
        // Other
        "Jesse Warford",
    };
    const unsigned VIP_COUNT = sizeof(vips) / sizeof(char *);
    for (unsigned i = 0; i < VIP_COUNT; i++)
    {
        unsigned swap = (VIP_COUNT * (get_rand_32() & 0xFFFF)) >> 16;
        char *tmp = vips[i];
        vips[i] = vips[swap];
        vips[swap] = tmp;
    }
    printf("          Patrons - %s", vips[0]);
    unsigned col = 20 + strlen(vips[0]) + 2;
    for (unsigned i = 1; i < VIP_COUNT - 1; i++)
    {
        printf(", ");
        col += strlen(vips[i]) + 2;
        if (col > 78)
        {
            col = 20 + strlen(vips[i]) + 2;
            printf("\n%20s", "");
        }
        printf("%s", vips[i]);
    }
    puts(".");
}
