# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# Cross toolchain: ARM Cortex-M33 (EFR32MG21), Thumb, freestanding.
#
# Separate from cmake/arm-none-eabi.cmake on purpose: the core differs, so the
# flags differ, and the project-wide toolchain file is chosen per board at
# configure time (docs/boards.md §4). Startup is supplied by the board's
# DS_STARTUP, not by newlib.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_PREFIX arm-none-eabi-)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_OBJCOPY      ${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_SIZE         ${TOOLCHAIN_PREFIX}size)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MCU_FLAGS "-mcpu=cortex-m33 -mthumb")
set(CMAKE_C_FLAGS_INIT    "${MCU_FLAGS} -Os -std=c99 -ffunction-sections -fdata-sections -fno-common")
set(CMAKE_ASM_FLAGS_INIT  "${MCU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${MCU_FLAGS} -nostdlib -Wl,--gc-sections")
