/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#pragma once

static const char EMU_CREDITS[] =
    "Picocomputer 6502 emulator - credits and third-party notices\n"
    "============================================================\n"
    "\n"
    "  RP6502                       BSD-3   (c) 2026 Rumbledethumps;\n"
    "  Pi Pico SDK                  BSD-3   (c) 2020 Raspberry Pi (Trading) Ltd.\n"
    "  floooh/chips                 zlib    (c) 2018 Andre Weissflog\n"
#ifdef EMU_WITH_SOKOL
    "  floooh/sokol                 zlib    (c) 2018 Andre Weissflog\n"
#endif
    "  emu8950 (OPL audio)          MIT     (c) 2001-2020 Mitsutaka Okazaki;\n"
    "                                       (c) 2021-2022 Graham Sanderson\n"
    "  FatFs (ffunicode.c), ChaN    FatFs   (c) 2025 ChaN\n"
#ifdef EMU_WITH_DEBUGGER
    "  Dear ImGui                   MIT     (c) 2014-2026 Omar Cornut\n"
    "  cppdap                       Apache  (c) 2019 Google LLC\n"
    "  nlohmann/json (in cppdap)    MIT     (c) 2013-2019 Niels Lohmann\n"
#endif
#ifdef _WIN32
    "  wingetopt                    ISC + BSD-2  (c) 2002 Todd C. Miller;\n"
    "                                       (c) 2000 The NetBSD Foundation\n"
#endif
#if defined(__clang__) && !defined(__EMSCRIPTEN__)
    "  Clang/LLVM                   Apache  (c) the LLVM Project\n"
#elif defined(__GNUC__) && !defined(__EMSCRIPTEN__)
    "  GCC                          GPLv3   (c) Free Software Foundation, Inc.\n"
#endif
#ifdef __EMSCRIPTEN__
    "  Emscripten                   MIT     (c) 2010-2014 Emscripten authors\n"
    "  musl libc                    MIT     (c) 2005-2020 Rich Felker et al.\n"
#endif
    "\n"
    "------------------------------------------------------------\n"
    "BSD 3-Clause License  (applies to the BSD-3 components listed above)\n"
    "------------------------------------------------------------\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions are met:\n"
    "1. Redistributions of source code must retain the above copyright notice,\n"
    "   this list of conditions and the following disclaimer.\n"
    "2. Redistributions in binary form must reproduce the above copyright notice,\n"
    "   this list of conditions and the following disclaimer in the documentation\n"
    "   and/or other materials provided with the distribution.\n"
    "3. Neither the name of the copyright holder nor the names of its contributors\n"
    "   may be used to endorse or promote products derived from this software\n"
    "   without specific prior written permission.\n"
    "THIS SOFTWARE IS PROVIDED \"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES ARE\n"
    "DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE\n"
    "FOR ANY DAMAGES ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE.\n"
    "\n"
    "------------------------------------------------------------\n"
    "MIT License  (applies to the MIT components listed above)\n"
    "------------------------------------------------------------\n"
    "Permission is hereby granted, free of charge, to any person obtaining a copy\n"
    "of this software and associated documentation files (the \"Software\"), to deal\n"
    "in the Software without restriction, including without limitation the rights\n"
    "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n"
    "copies of the Software, and to permit persons to whom the Software is\n"
    "furnished to do so, subject to the following conditions:\n"
    "The above copyright notice and this permission notice shall be included in\n"
    "all copies or substantial portions of the Software.\n"
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND.\n"
    "\n"
    "------------------------------------------------------------\n"
    "zlib License  (applies to the zlib components listed above)\n"
    "------------------------------------------------------------\n"
    "This software is provided 'as-is', without any express or implied warranty.\n"
    "Permission is granted to anyone to use this software for any purpose,\n"
    "including commercial applications, and to alter it and redistribute it\n"
    "freely, subject to the following restrictions: 1. The origin of this software\n"
    "must not be misrepresented. 2. Altered source versions must be plainly marked\n"
    "as such. 3. This notice may not be removed from any source distribution.\n"
    "\n"
#if defined(EMU_WITH_DEBUGGER) || (defined(__clang__) && !defined(__EMSCRIPTEN__))
    "------------------------------------------------------------\n"
    "Apache License 2.0  (applies to the Apache components listed above)\n"
    "------------------------------------------------------------\n"
    "Licensed under the Apache License, Version 2.0; full text at\n"
    "https://www.apache.org/licenses/LICENSE-2.0\n"
    "Distributed on an \"AS IS\" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.\n"
    "\n"
#endif
#ifdef _WIN32
    "------------------------------------------------------------\n"
    "ISC and BSD 2-Clause  (wingetopt)\n"
    "------------------------------------------------------------\n"
    "Permission to use, copy, modify, and distribute this software for any purpose\n"
    "with or without fee is hereby granted, provided that the above copyright\n"
    "notice and this permission notice appear in all copies. THE SOFTWARE IS\n"
    "PROVIDED \"AS IS\" AND THE AUTHOR DISCLAIMS ALL WARRANTIES. The bundled NetBSD\n"
    "getopt is BSD 2-Clause, full text at\n"
    "https://opensource.org/license/bsd-2-clause.\n"
    "\n"
#endif
#if defined(__GNUC__) && !defined(__clang__)
    "------------------------------------------------------------\n"
    "GNU GPL v3 + Runtime Library Exception  (applies to GCC, listed above)\n"
    "------------------------------------------------------------\n"
    "GCC (the GNU Compiler Collection) is licensed under the GNU General Public\n"
    "License v3 with the GCC Runtime Library Exception; full texts at\n"
    "https://www.gnu.org/licenses/ (gpl-3.0.html, gcc-exception-3.1.html).\n"
    "\n"
#endif
    "------------------------------------------------------------\n"
    "FatFs License  (ffunicode.c)\n"
    "------------------------------------------------------------\n"
    "Copyright (C) 2025, ChaN, all right reserved. FatFs is open source software.\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that source-code redistributions retain\n"
    "the above copyright notice, this condition and the following disclaimer. This\n"
    "software is provided by the copyright holder and contributors \"AS IS\" and any\n"
    "warranties related to this software are DISCLAIMED. Binary redistribution\n"
    "needs no documentation notice.\n";

#undef EMU_CREDITS_STR
#undef EMU_CREDITS_STR2
