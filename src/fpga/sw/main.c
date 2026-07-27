/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's runner. This grows into the counterpart of ria/main.c; so
 * far it does what the RIA does at boot: load a program into the 6502's
 * memory, write its vectors into the register cells, and release its reset.
 */

#include "ria/api/api.h"
#include "ria/main.h"
#include "ria/sys/cpu.h"
#include "ria/sys/ria.h"

#include <stdint.h>

#define MMIO_CONSOLE (*(volatile uint32_t *)0xF0000000u)

/* The machine, as mapped in rp6502.sv. Byte windows by design. */
#define SRAM ((volatile uint8_t *)0x10000000u)
#define REGS_WIN ((volatile uint8_t *)0x20000000u)
#define CPU_RUN (*(volatile uint8_t *)0x40000000u)
#define API_PENDING (*(volatile uint8_t *)0x40000004u)

static void print(const char *s)
{
    while (*s)
        MMIO_CONSOLE = (uint8_t)*s++;
}

/* Until the .rp6502 loader arrives, the program rides in the firmware:
 * print "HELLO, WORLD!\r\n" through $FFE1 under the $FFE0 ready bit, STP. */
static const uint8_t boot_prog[] = {
    0xA2, 0x00,              /*       ldx #0          */
    0xBD, 0x22, 0x02,        /* loop: lda msg,x       */
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
    0xDB,                    /*       stp             */
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

static bool api_answered;

bool main_api(uint8_t operation)
{
    print("api: op\n");
    api_answered = true;
    switch (operation)
    {
    case 0x42:
        return api_return_ax(0x4143);
    default:
        return api_return_errno(API_ENOSYS);
    }
}

int main(void)
{
    print("boot: loading\n");

    for (uint32_t i = 0; i < sizeof boot_prog; i++)
        SRAM[BOOT_ORG + i] = boot_prog[i];

    /* Verify through the same window before trusting it. */
    for (uint32_t i = 0; i < sizeof boot_prog; i++)
        if (SRAM[BOOT_ORG + i] != boot_prog[i])
        {
            print("boot: verify failed\n");
            return 1;
        }

    /* Vectors live in the register cells; RESB releases the 6502. */
    REGS_WIN[0x1C] = BOOT_ORG & 0xFF;
    REGS_WIN[0x1D] = BOOT_ORG >> 8;
    CPU_RUN = 1;

    api_run();
    print("boot: running\n");

    /* The OS loop: the real api.c latches the op and dispatches through
     * main_api until the handler finishes. */
    for (uint32_t spins = 0; spins < 2000000u; spins++)
    {
        if (API_PENDING)
            API_PENDING = 0;
        api_task();
        if (api_answered)
            break;
    }
    return 0;
}
