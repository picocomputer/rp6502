/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Test fixture: built to tests/roms/vars.elf to exercise the DWARF type/line
 * readers. Not part of any distributed executable.
 */

#include <stdint.h>

struct point { int x; int y; char tag; };
enum color { RED=0, GREEN=1, BLUE=7 };

char gchar = 'A';
unsigned char guchar = 200;
int gint = -1234;
unsigned int guint = 50000;
long glong = 1234567;
char gstr[8] = "hi";
int garr[4] = {10,20,30,40};
struct point gpt = {3, 4, 'Z'};
enum color gcol = BLUE;
char *gptr = gstr;
struct point *gpp = &gpt;

volatile int sink;

int main(void) {
    int local_i = 42;
    struct point lp = {1,2,'x'};
    char *lpc = gstr;
    for (local_i=0; local_i<10; local_i++) sink += lp.x + (*lpc);
    return sink;
}
