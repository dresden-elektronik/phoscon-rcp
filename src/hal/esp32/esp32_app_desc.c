/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_app_desc.c — the app descriptor ESP-IDF's bootloader requires at the start of an app's first
 * flash segment (docs/hardware_esp32.md §12.3).

On a part whose MMU page size is configurable the bootloader reads the descriptor before it maps
anything, refuses an image without its magic, takes the page size from it, and checks the eFuse block
revision range against the chip; the linker script places it first in the read-only window. The layout
is ESP-IDF's esp_app_desc_t, 256 bytes; esptool's elf2image writes the ELF's SHA-256 into it. The time
and date fields stay empty so that an image is a function of its sources. */
#include <stdint.h>

#include "phoscon_version.h"

#define APP_DESC_MAGIC 0xABCD5432u
/* log2 of the 64 KiB MMU page the linker script lays the flash windows out in. */
#define APP_DESC_MMU_PAGE_LOG2 16u
/* The eFuse block revisions an image accepts, major * 100 + minor: every 0.x part. */
#define APP_DESC_EFUSE_BLK_MIN 0u
#define APP_DESC_EFUSE_BLK_MAX 99u

typedef struct
{
    uint32_t magic;
    uint32_t secureVersion;
    uint32_t reserved1[2];
    char version[32];
    char projectName[32];
    char time[16];
    char date[16];
    char idfVersion[32];
    uint8_t elfSha256[32];
    uint16_t minEfuseBlockRevision;
    uint16_t maxEfuseBlockRevision;
    uint8_t mmuPageSizeLog2;
    uint8_t reserved3[3];
    uint32_t reserved2[18];
} Esp32_AppDesc;

/* C99 has no static assertion: a negative array size is the compile error. */
typedef char esp32AppDescIs256Bytes[(sizeof(Esp32_AppDesc) == 256u) ? 1 : -1];

__attribute__((section(".flash.appdesc"), used)) const Esp32_AppDesc esp32AppDesc = {
    APP_DESC_MAGIC,
    0u,
    {0u, 0u},
    PHOSCON_RCP_VERSION,
    "phoscon-rcp",
    "",
    "",
    "",
    {0u},
    APP_DESC_EFUSE_BLK_MIN,
    APP_DESC_EFUSE_BLK_MAX,
    APP_DESC_MMU_PAGE_LOG2,
    {0u, 0u, 0u},
    {0u}};
