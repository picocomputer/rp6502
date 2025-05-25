#ifndef _CYW43_CONFIG_H
#define _CYW43_CONFIG_H

#include <cyw43_configport.h>

#define CYW43_RESOURCE_ATTRIBUTE __attribute__((aligned(4))) __in_flash("cyw43firmware")
#define CYW43_PIO_CLOCK_DIV_DYNAMIC 1

#endif
