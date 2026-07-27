/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's runner. This grows into the counterpart of ria/main.c; so
 * far it does what the RIA does at boot: load a program into the 6502's
 * memory, write its vectors into the register cells, and release its reset.
 */

#include "com.h"
#include "mmio.h"
#include "rom.h"
#include "vid.h"
#include "ria/api/api.h"
#include "ria/api/std.h"
#include "ria/main.h"
#include "ria/str/rln.h"
#include "ria/sys/cpu.h"
#include "ria/sys/ria.h"
#include "vga/term/term.h"

#include <stdint.h>

static void print(const char *s)
{
    while (*s)
        MMIO_CONSOLE = (uint8_t)*s++;
}

/* Until the .rp6502 loader arrives, the program rides in the firmware:
 * print "HELLO, WORLD!\r\n" through $FFE1 under the $FFE0 ready bit, then a
 * bare syscall, then one carrying an xstack argument, then echo the console
 * input back until a carriage return arrives, STP. */
static const uint8_t boot_prog[] = {
    0xA2, 0x00,              /*       ldx #0          */
    0xBD, 0x73, 0x02,        /* loop: lda msg,x       */
    0xF0, 0x0C,              /*       beq done        */
    0x2C, 0xE0, 0xFF,        /* wait: bit $FFE0       */
    0x10, 0xFB,              /*       bpl wait        */
    0x8D, 0xE1, 0xFF,        /*       sta $FFE1       */
    0xE8,                    /*       inx             */
    0xD0, 0xF0,              /*       bne loop        */
    0xEA,                    /*       nop             */
    /* done: the machine's first syscall — op $42 answers AX */
    0xA9, 0x42,              /*       lda #$42        */
    0x8D, 0xEF, 0xFF,        /*       sta $FFEF       */
    0x20, 0xF1, 0xFF,        /*       jsr $FFF1       */
    0x8D, 0xE1, 0xFF,        /*       sta $FFE1       */
    0x8E, 0xE1, 0xFF,        /*       stx $FFE1       */
    /* op $43 increments the uint16 pushed on the xstack */
    0xA9, 0x42,              /*       lda #$42        */
    0x8D, 0xEC, 0xFF,        /*       sta $FFEC       */
    0xA9, 0x43,              /*       lda #$43        */
    0x8D, 0xEC, 0xFF,        /*       sta $FFEC       */
    0xA9, 0x43,              /*       lda #$43        */
    0x8D, 0xEF, 0xFF,        /*       sta $FFEF       */
    0x20, 0xF1, 0xFF,        /*       jsr $FFF1       */
    0x8D, 0xE1, 0xFF,        /*       sta $FFE1       */
    0x8E, 0xE1, 0xFF,        /*       stx $FFE1       */
    /* echo the RX latch back out until a carriage return */
    0x2C, 0xE0, 0xFF,        /* echo: bit $FFE0       */
    0x50, 0xFB,              /*       bvc echo        */
    0xAD, 0xE2, 0xFF,        /*       lda $FFE2       */
    0x48,                    /*       pha             */
    0x2C, 0xE0, 0xFF,        /* wtx:  bit $FFE0       */
    0x10, 0xFB,              /*       bpl wtx         */
    0x68,                    /*       pla             */
    0x8D, 0xE1, 0xFF,        /*       sta $FFE1       */
    0xC9, 0x0D,              /*       cmp #$0D        */
    0xD0, 0xEA,              /*       bne echo        */
    /* read a line from stdin — the genuine std/rln engine — and print it */
    0xA9, 0x00,              /*       lda #0          */
    0x8D, 0xF4, 0xFF,        /*       sta $FFF4       ; API_A = fd 0 */
    0xA9, 0x00,              /*       lda #0          */
    0x8D, 0xEC, 0xFF,        /*       sta $FFEC       ; count hi */
    0xA9, 0x20,              /*       lda #$20        */
    0x8D, 0xEC, 0xFF,        /*       sta $FFEC       ; count lo */
    0xA9, 0x16,              /*       lda #$16        */
    0x8D, 0xEF, 0xFF,        /*       sta $FFEF       ; op read_xstack */
    0x20, 0xF1, 0xFF,        /*       jsr $FFF1       */
    0xAA,                    /*       tax             ; bytes read */
    0xF0, 0x09,              /*       beq rdone       */
    0xAD, 0xEC, 0xFF,        /* rloop:lda $FFEC       */
    0x8D, 0xE1, 0xFF,        /*       sta $FFE1       */
    0xCA,                    /*       dex             */
    0xD0, 0xF7,              /*       bne rloop       */
    0xDB,                    /* rdone:stp             */
    'H', 'E', 'L', 'L', 'O', ',', ' ',
    'W', 'O', 'R', 'L', 'D', '!', '\r', '\n', 0,
};

#define BOOT_ORG 0x0200u

/* The machine's lifecycle contract, minimally. */
bool cpu_active(void)
{
    return CPU_RUN != 0;
}

bool ria_active(void)
{
    return false;
}

const std_driver_t *main_std_drivers(size_t *count)
{
    *count = 0;
    return 0;
}

bool main_api(uint8_t operation)
{
    switch (operation)
    {
    case 0x14:
        return std_api_open();
    case 0x15:
        return std_api_close();
    case 0x16:
        return std_api_read_xstack();
    case 0x18:
        return std_api_write_xstack();
    case 0x1A:
        return std_api_lseek_cc65();
    case 0x1D:
        return std_api_lseek_llvm();
    case 0x1E:
        return std_api_syncfs();
    case 0x30:
        return rln_api_lastkey();
    case 0x31:
        return rln_api_peek();
    case 0x32:
        return rln_api_poke();
    case 0x42:
        return api_return_ax(0x4143);
    case 0x43:
    {
        uint16_t val;
        if (!api_pop_uint16_end(&val))
            return api_return_errno(API_EINVAL);
        return api_return_ax(val + 1);
    }
    default:
        return api_return_errno(API_ENOSYS);
    }
}

int main(void)
{
    /* No str_init: it exists to apply a locale, and this machine has one
     * locale and no S() callers — the whole localized chain is meant to
     * collect under --gc-sections. */
    com_init();
    std_init();
    rln_init();
    term_init();
    {
        /* Install the console the way the VGA chip's xreg path would. */
        uint16_t xregs[5] = {0, 0, 0, 0, 0};
        term_prog(xregs);
    }
    print("boot: loading\n");

    /* A staged .rp6502 outranks the built-in program: the loader parses
     * it straight out of the platform's staging window. */
    uint32_t slot = MMIO_SLOT;
    bool staged = false;
    if (slot)
    {
        staged = rom_load_staged(slot);
        MMIO_SLOT = 0;
        if (!staged)
        {
            print("rom: bad image\n");
            return 1;
        }
        print("rom: staged\n");
    }

    if (!staged)
    {
        for (uint32_t i = 0; i < sizeof boot_prog; i++)
            SRAM[BOOT_ORG + i] = boot_prog[i];

        /* Verify through the same window before trusting it. */
        for (uint32_t i = 0; i < sizeof boot_prog; i++)
            if (SRAM[BOOT_ORG + i] != boot_prog[i])
            {
                print("boot: verify failed\n");
                return 1;
            }

        /* Vectors live in the register cells. */
        REGS_WIN[0x1C] = BOOT_ORG & 0xFF;
        REGS_WIN[0x1D] = BOOT_ORG >> 8;
    }

    /* The run fan-out precedes the 6502's release, in the firmware's
     * order. */
    com_run();
    rln_run();
    api_run();
    CPU_RUN = 1;
    print("boot: running\n");

    /* The OS loop, in the firmware's task order with api last. The real
     * api.c latches the op and dispatches through main_api; the manifold
     * moves the console bytes. Quiet after the syscalls means the work is
     * done — a simulation-only exit. */
    uint32_t quiet = 0;
    uint32_t moved = com_moved();
    for (uint32_t spins = 0; spins < 8000000u; spins++)
    {
        if (API_PENDING)
            API_PENDING = 0;
        std_task();
        com_task();
        rln_task();
        term_task();
        vid_task();
        api_task();
        if (com_moved() != moved)
        {
            moved = com_moved();
            quiet = 0;
        }
        else if (!API_BUSY && ++quiet > 2000)
        {
            break;
        }
    }
    return 0;
}
