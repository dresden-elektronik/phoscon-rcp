/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* string.h — the three functions RAIL's headers and archive use, declared against this tree's own
 * definitions (hal/mg21/rail/rail_shims.c). rail.h includes <string.h> for the inline
 * RAIL_CopyRxPacket; a build with no libc has no such header, and this one stands in for it for the
 * RAIL adapter's translation unit only (docs/hardware_mg21.md §10.6). */
#ifndef RAIL_COMPAT_STRING_H
#define RAIL_COMPAT_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

#endif /* RAIL_COMPAT_STRING_H */
