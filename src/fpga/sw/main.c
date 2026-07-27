/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's runner. This grows into the counterpart of ria/main.c; so
 * far it does what the RIA does at boot: load a program into the 6502's
 * memory, write its vectors into the register cells, and release its reset.
 */

#include <stdint.h>

#define MMIO_CONSOLE (*(volatile uint32_t *)0xF0000000u)

/* The machine, as mapped in rp6502.sv. Byte windows by design. */
#define SRAM ((volatile uint8_t *)0x10000000u)
#define REGS ((volatile uint8_t *)0x20000000u)
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
    0xBD, 0x1F, 0x02,        /* loop: lda msg,x       */
    0xF0, 0x0C,              /*       beq done        */
    0x2C, 0xE0, 0xFF,        /* wait: bit $FFE0       */
    0x10, 0xFB,              /*       bpl wait        */
    0x8D, 0xE1, 0xFF,        /*       sta $FFE1       */
    0xE8,                    /*       inx             */
    0xD0, 0xF0,              /*       bne loop        */
    0xEA,                    /*       nop             */
    /* done: the machine's first syscall — op $42 answers A+1 */
    0xA9, 0x42,              /*       lda #$42        */
    0x8D, 0xEF, 0xFF,        /*       sta $FFEF       */
    0x20, 0xF1, 0xFF,        /*       jsr $FFF1       */
    0x8D, 0xE1, 0xFF,        /*       sta $FFE1       */
    0xDB,                    /*       stp             */
    'H', 'E', 'L', 'L', 'O', ',', ' ',
    'W', 'O', 'R', 'L', 'D', '!', '\r', '\n', 0,
};

#define BOOT_ORG 0x0200u

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
    REGS[0x1C] = BOOT_ORG & 0xFF;
    REGS[0x1D] = BOOT_ORG >> 8;
    CPU_RUN = 1;

    print("boot: running\n");

    /* The OS loop, in miniature: answer syscalls until the 6502 stops.
     * Op $42 returns op+1 in A, by patching the trampoline the way
     * api_set_ax and api_set_regs_released do. */
    for (uint32_t spins = 0; spins < 2000000u; spins++)
    {
        if (!API_PENDING)
            continue;
        uint8_t op = REGS[0x0F];
        print("api: op\n");
        REGS[0x14] = (uint8_t)(op + 1); /* $FFF4 LDA operand */
        REGS[0x15] = 0xA2;              /* $FFF5 LDX #       */
        REGS[0x16] = 0x00;              /* $FFF6 operand     */
        REGS[0x17] = 0x60;              /* $FFF7 RTS         */
        REGS[0x13] = 0xA9;              /* $FFF3 LDA #       */
        REGS[0x12] = 0x00;              /* unblock           */
        API_PENDING = 0;
        break;
    }
    return 0;
}
