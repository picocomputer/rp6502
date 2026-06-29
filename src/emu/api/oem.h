/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * OEM code page (oem.c) — drives the font glyphs, clk strftime, and ff_uni2oem
 * filename conversion. Effective page only (default 437). The clk/atr handlers
 * reach the "run" names through the firmware api/oem.h; only these emu entry
 * points live here.
 */

#ifndef _EMU_OEM_H_
#define _EMU_OEM_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

void oem_reset(void);                /* reset to the default code page (437) */
uint16_t oem_get_code_page(void);    /* the active OEM code page */
bool oem_set_code_page(uint16_t cp); /* set it (font + clk); false if unsupported */

/* The firmware oem.h "run" names the vendored atr.c uses. No config-vs-running
 * split in the emulator, so these are the effective page (best-effort set). */
uint16_t oem_get_code_page_run(void);
void oem_set_code_page_run(uint16_t cp);

/* One UTF-8 sequence -> one OEM byte in the active code page (firmware
 * str_utf8_to_oem): ASCII passes through, a valid codepoint maps via ff_uni2oem,
 * anything unmappable or malformed becomes 0x7F. Advances *p past the sequence;
 * returns 0 only at the terminating NUL. */
unsigned char oem_utf8_to_oem(const char **p);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_OEM_H_ */
