# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# Cross toolchain: the RV32IMAC core of Espressif's 802.15.4 parts (the ESP32-C6 of the xiaoc6 target),
# freestanding.
#
# The compiler is Espressif's riscv32-esp-elf, the build ESP-IDF pins (tools/tools.json), because the
# PHY archives the rcp image will link are built with it (docs/hardware_esp32.md). It is looked for on
# PATH and in ~/opt/riscv32-esp-elf-<version>/riscv32-esp-elf/bin, where its release archive unpacks;
# -DDS_RISCV_ESP_BIN=<dir> names another. The startup is the board's DS_STARTUP
# (startup/startup_esp32.S); the image links no libc, only libgcc.
#
# The app image the board's bootloader loads is written from the ELF by esptool's elf2image
# (targets/<board>/board.cmake): a flat objcopy of an image whose code is mapped at 0x42000000 and whose
# RAM starts at 0x40800000 would be 32 MB of padding.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)

set(DS_RISCV_ESP_VERSION "16.1.0_20260609")
set(DS_RISCV_ESP_HINTS
  ${DS_RISCV_ESP_BIN}
  $ENV{HOME}/opt/riscv32-esp-elf-${DS_RISCV_ESP_VERSION}/riscv32-esp-elf/bin)

find_program(DS_RISCV_GCC NAMES riscv32-esp-elf-gcc HINTS ${DS_RISCV_ESP_HINTS})
find_program(DS_RISCV_OBJCOPY NAMES riscv32-esp-elf-objcopy HINTS ${DS_RISCV_ESP_HINTS})
find_program(DS_RISCV_SIZE NAMES riscv32-esp-elf-size HINTS ${DS_RISCV_ESP_HINTS})

if(NOT DS_RISCV_GCC OR NOT DS_RISCV_OBJCOPY)
  message(FATAL_ERROR "riscv32-esp-elf-gcc not found: unpack Espressif's crosstool-NG release "
                      "esp-${DS_RISCV_ESP_VERSION} into ~/opt/riscv32-esp-elf-${DS_RISCV_ESP_VERSION}/, or pass "
                      "-DDS_RISCV_ESP_BIN=<dir> (docs/build-and-flash.md §13)")
endif()

set(CMAKE_C_COMPILER   ${DS_RISCV_GCC})
set(CMAKE_ASM_COMPILER ${DS_RISCV_GCC})
set(CMAKE_OBJCOPY      ${DS_RISCV_OBJCOPY})
set(CMAKE_SIZE         ${DS_RISCV_SIZE})

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MCU_FLAGS "-march=rv32imac_zicsr_zifencei -mabi=ilp32")
set(CMAKE_C_FLAGS_INIT    "${MCU_FLAGS} -Os -std=c99 -ffreestanding -ffunction-sections -fdata-sections -fno-common")
set(CMAKE_ASM_FLAGS_INIT  "${MCU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${MCU_FLAGS} -nostartfiles -nostdlib -Wl,--gc-sections")
