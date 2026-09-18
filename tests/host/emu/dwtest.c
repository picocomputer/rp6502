/*
 * This file is the source of dwtest.elf, the DWARF 5 fixture that test_dwarf5
 * and test_dwarf_frame read. The fixture is an -O0 -g build from johnwbyrd's
 * llvm-mos fork, and its DW_AT_producer names the commit.
 *
 * test_dwarf5 uses line numbers from this file, so moving any line here, a
 * comment line included, means rebuilding dwtest.elf and updating those
 * line numbers and any asserted address that changes in the rebuild. In the
 * fixture and in this file, line 81 is main's opening brace.
 *
 * Inspect the fixture with:
 *   llvm-dwarfdump --all dwtest.elf
 *   llvm-dwarfdump --debug-frame dwtest.elf
 */

#include <stdint.h>

typedef struct
{
    int16_t x;
    int16_t y;
} Point;

typedef struct
{
    Point origin;
    uint16_t w;
    uint16_t h;
    char tag;
} Rect;

typedef enum
{
    RED,
    GREEN = 1,
    BLUE = 7,
} Color;

/* These globals are located by DW_OP_addrx through .debug_addr. */
int8_t g_i8 = -7;
uint8_t g_u8 = 200;
int16_t g_i16 = -1234;
uint16_t g_u16 = 55000;
char g_msg[8] = "hello";
Rect g_rect = {{3, 4}, 20, 10, 'R'};

/* dwarf_info.c reports these as DW_KIND_ENUM and DW_KIND_POINTER types. */
Color g_color = BLUE;
char *g_ptr = &g_msg[0];
Rect *g_rectp = &g_rect;

/* area's parameters and locals are DW_OP_fbreg locations on the soft stack. */
static int16_t area(Point a, Point b)
{
    int16_t dx = (int16_t)(b.x - a.x);
    int16_t dy = (int16_t)(b.y - a.y);
    int16_t s = (int16_t)(dx * dy);
    return s;
}

/* measure calls area, so area has a caller frame for the unwinder. */
static int16_t measure(Rect *r)
{
    Point tl = r->origin;
    Point br;
    br.x = (int16_t)(r->origin.x + (int16_t)r->w);
    br.y = (int16_t)(r->origin.y + (int16_t)r->h);
    return area(tl, br);
}

/* sum_to recurses, so each depth has its own rest at a different address. */
static uint16_t sum_to(uint16_t n)
{
    if (n == 0)
        return 0;
    uint16_t rest = sum_to((uint16_t)(n - 1));
    return (uint16_t)(n + rest);
}

int main(void)
{
    volatile int16_t a = measure(g_rectp);
    volatile uint16_t s = sum_to(5);
    return (int)(a + s + g_i8 + g_u8 + g_i16 + g_u16 + *g_ptr + g_rect.tag + g_color);
}
