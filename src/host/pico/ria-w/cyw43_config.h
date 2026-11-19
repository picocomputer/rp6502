#ifndef _CYW43_CONFIG_H
#define _CYW43_CONFIG_H

#include "core/sys/debug_log.h"
#define CYW43_DEBUG(...) RP6502_LOG(cyw43, DEBUG, __VA_ARGS__)
#define CYW43_INFO(...) RP6502_LOG(cyw43, INFO, __VA_ARGS__)
#define CYW43_WARN(...) RP6502_LOG(cyw43, WARN, __VA_ARGS__)

/* The driver allocates in one place: two buffers for as long as it takes to
 * load the Bluetooth firmware. net/cyw.c lends it buf1k for them. */
#include <stddef.h>
void *cyw_malloc(size_t size);
void cyw_free(void *ptr);
#define cyw43_malloc cyw_malloc
#define cyw43_free cyw_free

#include <cyw43_configport.h>

#define CYW43_RESOURCE_ATTRIBUTE __attribute__((aligned(4))) __in_flash("cyw43firmware")
#define CYW43_PIO_CLOCK_DIV_DYNAMIC 1

#endif
