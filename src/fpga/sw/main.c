/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's runner. This grows into the counterpart of ria/main.c; so
 * far it does what the RIA does at boot: load a program into the 6502's
 * memory, write its vectors into the register cells, and release its reset.
 */

#include "aud.h"
#include "com.h"
#include "font.h"
#include "kbd.h"
#include "main.h"
#include "mmio.h"
#include "rom.h"
#include "vga.h"
#include "vid.h"
#include "ria/api/api.h"
#include "ria/api/std.h"
#include "ria/main.h"
#include "ria/str/rln.h"
#include "ria/sys/cpu.h"
#include "ria/sys/pix.h"
#include "ria/sys/ria.h"
#include "vga/modes/mode1.h"
#include "vga/modes/mode2.h"
#include "vga/modes/mode3.h"
#include "vga/modes/mode4.h"
#include "vga/modes/mode5.h"
#include "vga/term/term.h"

#include <pico/rand.h>

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

bool ria_active(void)
{
    return false;
}

bool main_xreg_0(uint8_t channel, uint8_t address, uint16_t word)
{
    /* HID report blocks and audio; the rest arrive with their engines. */
    if (channel == 0 && address == 0)
        return kbd_set_xram(word);
    if (channel == 1 && address == 0)
        return aud_psg_xreg(word);
    return false;
}

/* The VGA mode-xreg accumulator, the emulator's 16 slots. */
static uint16_t main_xregs[16];

bool main_xreg_1(uint8_t channel, uint8_t address, uint16_t word)
{
    if (channel == 0)
    {
        main_xregs[address & 0x0F] = word;
        if (address == 0)
        {
            bool ok = vga_set_canvas(word);
            for (int i = 0; i < 16; i++)
                main_xregs[i] = 0;
            return ok;
        }
        if (address == 1)
        {
            bool ok;
            vga_prog_mode((uint8_t)word, main_xregs[2]);
            switch (word)
            {
            case 0:
                ok = vid_mode0_prog(main_xregs);
                break;
            case 1:
                ok = mode1_prog(main_xregs);
                break;
            case 2:
                ok = mode2_prog(main_xregs);
                break;
            case 3:
                ok = mode3_prog(main_xregs);
                break;
            case 4:
                ok = mode4_prog(main_xregs);
                break;
            case 5:
                ok = mode5_prog(main_xregs);
                break;
            default:
                ok = false;
                break;
            }
            for (int i = 0; i < 16; i++)
                main_xregs[i] = 0;
            return ok;
        }
        return true;
    }
    /* Channels 1-14 reach external bus devices with no ACK; none exist,
     * so a no-op success — the emulator's rule. */
    return true;
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
    case 0x01:
        return pix_api_xreg();
    case 0x02:
        /* atr_api_phi2's shape: a report, not a control. What sets the
         * clock is the Pocket's own menu. */
        return api_return_ax(cpu_get_phi2_khz_run());
    case 0x03:
        /* atr_api_code_page's shape. With no filesystem the code page is
         * the glyphs alone; the OEM conversion the string layer wants
         * arrives with its tables. */
        if (API_AX)
            font_set_code_page(API_AX);
        return api_return_ax(font_get_code_page());
    case 0x04:
        return api_return_axsreg(get_rand_64() & 0x7FFFFFFF);
    case 0x06:
        if (!api_set_errno_opt(API_A))
            return api_return_errno(API_EINVAL);
        return api_return_ax(0);
    case 0x0A:
        /* Attributes grow with their engines; these exist now. */
        switch (API_A)
        {
        case 0x00:
            return api_return_axsreg(api_get_errno_opt());
        case 0x03:
            return api_return_axsreg(rln_get_max_length());
        case 0x04:
            return api_return_axsreg(get_rand_64() & 0x7FFFFFFF);
        case 0x05:
            return api_return_axsreg(com_get_bel());
        case 0x09:
            return api_return_axsreg(rln_get_caps());
        case 0x0A:
            return api_return_axsreg(rln_get_term_width());
        case 0x0B:
            return api_return_axsreg(rln_get_term_height());
        case 0x0C:
            return api_return_axsreg(rln_get_suppress_nl());
        default:
            return api_return_errno(API_EINVAL);
        }
    case 0x0B:
    {
        uint32_t value;
        if (!api_pop_uint32_end(&value))
            return api_return_errno(API_EINVAL);
        if (value > 0x7FFFFFFF)
            return api_return_errno(API_EINVAL);
        switch (API_A)
        {
        case 0x00:
            if (value > UINT8_MAX || !api_set_errno_opt((uint8_t)value))
                return api_return_errno(API_EINVAL);
            break;
        case 0x03:
            if (value > UINT8_MAX)
                return api_return_errno(API_EINVAL);
            rln_set_max_length((uint8_t)value);
            break;
        case 0x05:
            if (value > 1)
                return api_return_errno(API_EINVAL);
            com_set_bel(value);
            break;
        case 0x09:
            if (value > 2)
                return api_return_errno(API_EINVAL);
            rln_set_caps((uint8_t)value);
            break;
        case 0x0A:
            if (value > UINT16_MAX)
                return api_return_errno(API_EINVAL);
            rln_set_term_width((uint16_t)value);
            break;
        case 0x0B:
            if (value > UINT16_MAX)
                return api_return_errno(API_EINVAL);
            rln_set_term_height((uint16_t)value);
            break;
        case 0x0C:
            if (value > 1)
                return api_return_errno(API_EINVAL);
            rln_set_suppress_nl((uint8_t)value);
            break;
        default:
            return api_return_errno(API_EINVAL);
        }
        return api_return_ax(0);
    }
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
    /* Checkpoints, not decoration. These are the only report the
     * hardware gives while it is coming up, and the two that follow the
     * drivers bracket the font copy out of SDRAM — the one init here
     * that waits on something off-chip. A silent log says the soft CPU
     * never ran; a log that stops says where. */
    print("boot: rv\n");
    com_init();
    std_init();
    rln_init();
    term_init();
    print("boot: term\n");
    vid_init();
    print("boot: loading\n");

    /* A staged .rp6502 outranks the built-in program: the loader parses
     * it straight out of the platform's staging window. */
    uint32_t slot = MMIO_SLOT;
    bool staged = false;
    bool runnable = true;
    if (slot)
    {
        staged = rom_load_staged(slot);
        MMIO_SLOT = 0;
        print(staged ? "rom: staged\n" : "rom: bad image\n");
        runnable = staged;
    }

    if (runnable && !staged)
    {
        for (uint32_t i = 0; i < sizeof boot_prog; i++)
            SRAM[BOOT_ORG + i] = boot_prog[i];

        /* Verify through the same window before trusting it. */
        for (uint32_t i = 0; i < sizeof boot_prog; i++)
            if (SRAM[BOOT_ORG + i] != boot_prog[i])
            {
                print("boot: verify failed\n");
                runnable = false;
                break;
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
    if (runnable)
    {
        CPU_RUN = 1;
        print("boot: running\n");
    }

    /* The OS loop, in the firmware's task order with api last. The real
     * api.c latches the op and dispatches through main_api; the
     * manifold moves the console bytes. It never ends: a machine's
     * firmware has nowhere to return to, and the simulation decides
     * for itself when a run is over. */
    for (;;)
    {
        if (API_PENDING)
        {
            /* ZXSTACK and EXIT run inside the $FFEF write on the RIA
             * and the emulator; here the pending strobe is that write. */
            API_PENDING = 0;
            if (API_OP == 0x00)
            {
                xstack_ptr = XSTACK_SIZE;
                api_return_ax(0);
            }
            else if (API_OP == 0xFF)
            {
                CPU_RUN = 0;
                api_return_ax(0);
            }
        }
        kbd_task();
        std_task();
        com_task();
        rln_task();
        term_task();
        vid_task();
        api_task();
    }
}
