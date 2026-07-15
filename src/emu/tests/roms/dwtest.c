/*
 * DWARF5 inspection fixture for the rp6502 emulator reader rewrite.
 * Exercises the location/type/unwind cases the reader must handle:
 *   - globals of several base types + an array + a struct instance
 *   - an enum type + pointer-typed globals (char * and Rect *)
 *   - a nested struct type (Point in Rect)
 *   - a function with parameters + locals (soft-stack fbreg locations)
 *   - a multi-frame call chain + a small recursion (CFI unwind + frame bases)
 * The committed dwtest.elf is the DWARF5 fixture for test_dwarf5 / test_dwarf_frame,
 * rebuilt with the llvm-mos debug fork (johnwbyrd, feature/debug/v*) as:
 *   mos-rp6502-clang -g -O0 dwtest.c -o dwtest.rp6502   # emits the sidecar dwtest.rp6502.elf
 * Inspect: llvm-dwarfdump --all / --debug-frame dwtest.rp6502.elf
 * If regenerated, the addresses asserted in test_dwarf5/test_dwarf_frame may shift.
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

/* globals: base types + array + aggregate (DW_OP_addr / DW_OP_addrx) */
int8_t g_i8 = -7;
uint8_t g_u8 = 200;
int16_t g_i16 = -1234;
uint16_t g_u16 = 55000;
char g_msg[8] = "hello";
Rect g_rect = {{3, 4}, 20, 10, 'R'};

/* enum + pointer types (DW_KIND_ENUM / DW_KIND_POINTER) */
Color g_color = BLUE;
char *g_ptr = &g_msg[0];
Rect *g_rectp = &g_rect;

/* leaf with params + locals (soft-stack frame, fbreg locations) */
static int16_t area(Point a, Point b)
{
    int16_t dx = (int16_t)(b.x - a.x);
    int16_t dy = (int16_t)(b.y - a.y);
    int16_t s = (int16_t)(dx * dy);
    return s;
}

/* one more frame so a backtrace has depth (CFI unwind) */
static int16_t measure(Rect *r)
{
    Point tl = r->origin;
    Point br;
    br.x = (int16_t)(r->origin.x + (int16_t)r->w);
    br.y = (int16_t)(r->origin.y + (int16_t)r->h);
    return area(tl, br);
}

/* small recursion: distinct locals at each depth */
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
