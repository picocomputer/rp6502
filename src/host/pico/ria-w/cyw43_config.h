#ifndef _CYW43_CONFIG_H
#define _CYW43_CONFIG_H

// Ahead of the port's defaults: the driver's lines are the cyw43 category.
// Its hex dumps print in pieces through CYW43_PRINTF and stay with it.
#include "core/sys/debug_log.h"
#define CYW43_DEBUG(...) RP6502_LOG(cyw43, DEBUG, __VA_ARGS__)
#define CYW43_INFO(...) RP6502_LOG(cyw43, INFO, __VA_ARGS__)
#define CYW43_WARN(...) RP6502_LOG(cyw43, WARN, __VA_ARGS__)

#include <cyw43_configport.h>

// Not alignas; the driver's firmware blobs expand this after the declarator,
// where an alignment specifier is a syntax error.
#define CYW43_RESOURCE_ATTRIBUTE __attribute__((aligned(4))) __in_flash("cyw43firmware")
#define CYW43_PIO_CLOCK_DIV_DYNAMIC 1

#endif
