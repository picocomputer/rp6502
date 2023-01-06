/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _ANSI_H_
#define _ANSI_H_

typedef enum
{
    ansi_state_C0,
    ansi_state_Fe,
    ansi_state_SS3,
    ansi_state_CSI
} ansi_state_t;

#define ANSI_CANCEL '\30'

#define ANSI_CSI_CODE(seq) "\33[" seq
#define ANSI_SGR_CODE(n) ANSI_CSI_CODE(#n "m")

#define ANSI_KEY_ARROW_RIGHT ANSI_CSI_CODE("C")
#define ANSI_KEY_ARROW_LEFT ANSI_CSI_CODE("D")
#define ANSI_KEY_DELETE "\33\x7f"

#define ANSI_FORWARD(n) ANSI_CSI_CODE(#n "C")
#define ANSI_BACKWARD(n) ANSI_CSI_CODE(#n "D")
#define ANSI_DELETE(n) ANSI_CSI_CODE(#n "P")

#define ANSI_RESET ANSI_SGR_CODE(0)
#define ANSI_BOLD ANSI_SGR_CODE(1)
#define ANSI_NORMAL ANSI_SGR_CODE(22)

#define ANSI_FG_BLACK ANSI_SGR_CODE(30)
#define ANSI_FG_RED ANSI_SGR_CODE(31)
#define ANSI_FG_GREEN ANSI_SGR_CODE(32)
#define ANSI_FG_YELLOW ANSI_SGR_CODE(33)
#define ANSI_FG_BLUE ANSI_SGR_CODE(34)
#define ANSI_FG_MAGENTA ANSI_SGR_CODE(35)
#define ANSI_FG_CYAN ANSI_SGR_CODE(36)
#define ANSI_FG_WHITE ANSI_SGR_CODE(37)

#define ANSI_BG_BLACK ANSI_SGR_CODE(40)
#define ANSI_BG_RED ANSI_SGR_CODE(41)
#define ANSI_BG_GREEN ANSI_SGR_CODE(42)
#define ANSI_BG_YELLOW ANSI_SGR_CODE(43)
#define ANSI_BG_BLUE ANSI_SGR_CODE(44)
#define ANSI_BG_MAGENTA ANSI_SGR_CODE(45)
#define ANSI_BG_CYAN ANSI_SGR_CODE(46)
#define ANSI_BG_WHITE ANSI_SGR_CODE(47)

#endif /* _ANSI_H_ */
