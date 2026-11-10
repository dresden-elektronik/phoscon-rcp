/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* phy_shims.c — what Espressif's PHY archives expect from their host, answered from scratch
 * (docs/hardware_esp32.md §12.5).

The archives reach outside themselves for five symbols that are neither theirs nor the mask ROM's: a byte
copy and fill, which a -nostdlib image does not have; two formatted-print routines, which this image has no
console for, so they print nothing and format nothing; and the crystal's frequency in MHz. The ROM's own
memcpy and printf are not used, so the image has one copy loop and no hidden output path. Nothing here
includes a libc header: the prototypes are the C library's, written with the compiler's size type. */
#include <stdint.h>

#include "esp32_regs.h"

void *memcpy(void *dst, const void *src, __SIZE_TYPE__ n);
void *memset(void *dst, int c, __SIZE_TYPE__ n);
int sprintf(char *out, const char *format, ...);
int phy_printf(const char *format, ...);
int rtc_clk_xtal_freq_get(void);

/* The loops must not be turned back into calls to themselves (docs/coding-rules.md §7). */
__attribute__((optimize("no-tree-loop-distribute-patterns"))) void *memcpy(void *dst, const void *src,
                                                                           __SIZE_TYPE__ n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (n-- != 0u)
        *d++ = *s++;

    return dst;
}

__attribute__((optimize("no-tree-loop-distribute-patterns"))) void *memset(void *dst, int c, __SIZE_TYPE__ n)
{
    uint8_t *d = (uint8_t *)dst;

    while (n-- != 0u)
        *d++ = (uint8_t)c;

    return dst;
}

/* An empty string, so a caller that prints its buffer prints nothing. */
int sprintf(char *out, const char *format, ...)
{
    (void)format;

    if (out != 0)
        out[0] = '\0';

    return 0;
}

int phy_printf(const char *format, ...)
{
    (void)format;
    return 0;
}

/* ESP-IDF's soc_xtal_freq_t is the frequency in MHz. */
int rtc_clk_xtal_freq_get(void)
{
    return (int)(ESP32_XTAL_HZ / 1000000u);
}
