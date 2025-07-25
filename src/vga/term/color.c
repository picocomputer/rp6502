/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "color.h"
#include "scanvideo/scanvideo.h"

// Black and white palette for 1bpp

const uint16_t color_2[2] = {
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 0),                                   // 0  ANSI Black
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(192, 192, 192), // 1  ANSI White
};

// This is the ANSI color palette, not the web safe palette.
// Note that (0)Black is transparent while (16)Grey0 is not.

//    0-  7:  standard colors (ESC [ 30–37 m)
//    8- 15:  high intensity colors (ESC [ 90–97 m)
//   16-231:  6 × 6 × 6 cube (216 colors)
//  232-255:  grayscale from dark to light in 24 steps

const uint16_t color_256[256] = {
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 0),                                   // 0  Black
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(205, 0, 0),     // 1  Red
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 205, 0),     // 2  Green
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(205, 205, 0),   // 3  Yellow
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 238),     // 4  Blue
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(205, 0, 205),   // 5  Magenta
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 205, 205),   // 6  Cyan
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(229, 229, 229), // 7  White
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(127, 127, 127), // 8  Bright Black
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 0, 0),     // 9  Bright Red
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 255, 0),     // 10 Bright Green
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 0),   // 11 Bright Yellow
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(92, 92, 255),   // 12 Bright Blue
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 0, 255),   // 13 Bright Magenta
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 255, 255),   // 14 Bright Cyan
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 255), // 15 Bright White
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 0),       // 16 Grey0
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 95),      // 17
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 135),     // 18
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 175),     // 19
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 215),     // 20
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 255),     // 21
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 95, 0),      // 22
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 95, 95),     // 23
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 95, 135),    // 24
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 95, 175),    // 25
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 95, 215),    // 26
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 95, 255),    // 27
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 135, 0),     // 28
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 135, 95),    // 29
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 135, 135),   // 30
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 135, 175),   // 31
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 135, 215),   // 32
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 135, 255),   // 33
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 175, 0),     // 34
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 175, 95),    // 35
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 175, 135),   // 36
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 175, 175),   // 37
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 175, 215),   // 38
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 175, 255),   // 39
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 215, 0),     // 40
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 215, 95),    // 41
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 215, 135),   // 42
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 215, 175),   // 43
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 215, 215),   // 44
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 215, 255),   // 45
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 255, 0),     // 46
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 255, 95),    // 47
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 255, 135),   // 48
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 255, 175),   // 49
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 255, 215),   // 50
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 255, 255),   // 51
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 0, 0),      // 52
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 0, 95),     // 53
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 0, 135),    // 54
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 0, 175),    // 55
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 0, 215),    // 56
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 0, 255),    // 57
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 95, 0),     // 58
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 95, 95),    // 59
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 95, 135),   // 60
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 95, 175),   // 61
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 95, 215),   // 62
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 95, 255),   // 63
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 135, 0),    // 64
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 135, 95),   // 65
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 135, 135),  // 66
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 135, 175),  // 67
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 135, 215),  // 68
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 135, 255),  // 69
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 175, 0),    // 70
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 175, 95),   // 71
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 175, 135),  // 72
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 175, 175),  // 73
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 175, 215),  // 74
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 175, 255),  // 75
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 215, 0),    // 76
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 215, 95),   // 77
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 215, 135),  // 78
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 215, 175),  // 79
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 215, 215),  // 80
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 215, 255),  // 81
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 255, 0),    // 82
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 255, 95),   // 83
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 255, 135),  // 84
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 255, 175),  // 85
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 255, 215),  // 86
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(95, 255, 255),  // 87
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 0, 0),     // 88
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 0, 95),    // 89
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 0, 135),   // 90
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 0, 175),   // 91
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 0, 215),   // 92
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 0, 255),   // 93
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 95, 0),    // 94
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 95, 95),   // 95
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 95, 135),  // 96
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 95, 175),  // 97
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 95, 215),  // 98
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 95, 255),  // 99
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 135, 0),   // 100
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 135, 95),  // 101
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 135, 135), // 102
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 135, 175), // 103
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 135, 215), // 104
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 135, 255), // 105
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 175, 0),   // 106
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 175, 95),  // 107
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 175, 135), // 108
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 175, 175), // 109
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 175, 215), // 110
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 175, 255), // 111
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 215, 0),   // 112
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 215, 95),  // 113
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 215, 135), // 114
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 215, 175), // 115
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 215, 215), // 116
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 215, 255), // 117
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 255, 0),   // 118
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 255, 95),  // 119
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 255, 135), // 120
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 255, 175), // 121
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 255, 215), // 122
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(135, 255, 255), // 123
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 0, 0),     // 124
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 0, 95),    // 125
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 0, 135),   // 126
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 0, 175),   // 127
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 0, 215),   // 128
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 0, 255),   // 129
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 95, 0),    // 130
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 95, 95),   // 131
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 95, 135),  // 132
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 95, 175),  // 133
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 95, 215),  // 134
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 95, 255),  // 135
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 135, 0),   // 136
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 135, 95),  // 137
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 135, 135), // 138
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 135, 175), // 139
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 135, 215), // 140
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 135, 255), // 141
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 175, 0),   // 142
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 175, 95),  // 143
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 175, 135), // 144
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 175, 175), // 145
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 175, 215), // 146
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 175, 255), // 147
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 215, 0),   // 148
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 215, 95),  // 149
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 215, 135), // 150
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 215, 175), // 151
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 215, 215), // 152
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 215, 255), // 153
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 255, 0),   // 154
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 255, 95),  // 155
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 255, 135), // 156
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 255, 175), // 157
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 255, 215), // 158
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(175, 255, 255), // 159
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 0, 0),     // 160
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 0, 95),    // 161
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 0, 135),   // 162
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 0, 175),   // 163
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 0, 215),   // 164
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 0, 255),   // 165
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 95, 0),    // 166
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 95, 95),   // 167
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 95, 135),  // 168
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 95, 175),  // 169
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 95, 215),  // 170
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 95, 255),  // 171
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 135, 0),   // 172
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 135, 95),  // 173
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 135, 135), // 174
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 135, 175), // 175
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 135, 215), // 176
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 135, 255), // 177
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 175, 0),   // 178
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 175, 95),  // 179
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 175, 135), // 180
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 175, 175), // 181
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 175, 215), // 182
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 175, 255), // 183
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 215, 0),   // 184
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 215, 95),  // 185
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 215, 135), // 186
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 215, 175), // 187
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 215, 215), // 188
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 215, 255), // 189
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 255, 0),   // 190
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 255, 95),  // 191
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 255, 135), // 192
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 255, 175), // 193
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 255, 215), // 194
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(215, 255, 255), // 195
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 0, 0),     // 196
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 0, 95),    // 197
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 0, 135),   // 198
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 0, 175),   // 199
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 0, 215),   // 200
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 0, 255),   // 201
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 95, 0),    // 202
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 95, 95),   // 203
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 95, 135),  // 204
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 95, 175),  // 205
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 95, 215),  // 206
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 95, 255),  // 207
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 135, 0),   // 208
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 135, 95),  // 209
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 135, 135), // 210
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 135, 175), // 211
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 135, 215), // 212
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 135, 255), // 213
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 175, 0),   // 214
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 175, 95),  // 215
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 175, 135), // 216
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 175, 175), // 217
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 175, 215), // 218
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 175, 255), // 219
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 215, 0),   // 220
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 215, 95),  // 221
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 215, 135), // 222
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 215, 175), // 223
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 215, 215), // 224
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 215, 255), // 225
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 0),   // 226
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 95),  // 227
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 135), // 228
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 175), // 229
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 215), // 230
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 255), // 231 Grey100
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(8, 8, 8),       // 232 Grey3
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(18, 18, 18),    // 233 Grey7
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(28, 28, 28),    // 234 Grey11
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(38, 38, 38),    // 235 Grey15
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(48, 48, 48),    // 236 Grey19
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(58, 58, 58),    // 237 Grey23
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(68, 68, 68),    // 238 Grey27
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(78, 78, 78),    // 239 Grey30
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(88, 88, 88),    // 240 Grey35
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(98, 98, 98),    // 241 Grey39
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(108, 108, 108), // 242 Grey42
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(118, 118, 118), // 243 Grey46
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(128, 128, 128), // 244 Grey50
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(138, 138, 138), // 245 Grey54
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(148, 148, 148), // 246 Grey58
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(158, 158, 158), // 247 Grey62
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(168, 168, 168), // 248 Grey66
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(178, 178, 178), // 249 Grey70
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(188, 188, 188), // 250 Grey74
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(198, 198, 198), // 251 Grey78
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(208, 208, 208), // 252 Grey82
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(218, 218, 218), // 253 Grey85
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(228, 228, 228), // 254 Grey89
    PICO_SCANVIDEO_ALPHA_MASK | PICO_SCANVIDEO_PIXEL_FROM_RGB8(238, 238, 238), // 255 Grey93
};
