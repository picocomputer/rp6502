/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <tusb.h>
#include <stdio.h>

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_USB)
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define CDC_MAX_DESC 8
#define CDC_BUF_SIZE 32

struct
{
    char rx[CDC_BUF_SIZE];
    char tx[CDC_BUF_SIZE];
    // TODO something that anchors to tinyusb
    // TODO other into termios like baud, bits, stop, flow control
} cdc_desc[CDC_MAX_DESC];

void cdc_task(void)
{
}

// return desc idx
int cdc_open(const char *name)
{
}

// return false if not open
bool cdc_close(int desc_idx)
{
}

// return -1 error, >=0 num returned
int cdc_rx(char *buf, size_t buf_size)
{
}

// return -1 error, >=0 num sent
int cdc_tx(const char *buf, size_t buf_size)
{
}
