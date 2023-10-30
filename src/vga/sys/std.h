/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _STD_H_
#define _STD_H_

#include <pico/stdlib.h>
#include <stdbool.h>

#define STD_UART_TX 4
#define STD_UART_RX 5
#define STD_UART_INTERFACE uart1
#define STD_UART_BAUDRATE 115200
// STD IN Buffering is handled by 32 byte UART FIFO
#define STD_IN_BUF_SIZE 8
// STD OUT Buffer matches full speed USB BULK_PACKET_SIZE
#define STD_OUT_BUF_SIZE 64

void std_init(void);
void std_reclock(void);
void std_task(void);
void std_reclock(void);

void std_set_break(bool en);

size_t std_in_free(void);
bool std_in_empty(void);
void std_in_write(char ch);

bool std_out_empty(void);
void std_out_write(char ch);
char std_out_peek(void);
char std_out_read(void);

#endif /* _STD_H_ */
