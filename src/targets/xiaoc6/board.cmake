# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# The Seeed Studio XIAO ESP32-C6 target: an experimental board outside the ConBee/RaspBee family
# (docs/boards.md §5), on the shared ESP32 layer, src/hal/esp32/, with the ESP32-C6's numbers from
# src/hal/esp32/c6/ (docs/hardware_esp32.md §12).
#
# Configure with -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/riscv32-esp.cmake. The board keeps its ESP-IDF
# bootloader and partition table; the image is an ESP-IDF-format app for the ota_0 slot at 0x10000,
# running from flash, written from the ELF by esptool's elf2image and flashed by tools/esp32_flash.py
# (docs/build-and-flash.md §13).
#
# What this board builds today: the probe image boots, takes over the watchdogs, keeps the millisecond on
# SYSTIMER and publishes its records on the USB Serial/JTAG port. The radio is not driven yet; the rcp
# image is not offered.
if(DEFINED DS_MODE AND DS_MODE STREQUAL "rcp")
  message(FATAL_ERROR "xiaoc6: the rcp image needs the radio driver, which this board does not have yet "
                      "(docs/hardware_esp32.md §12). Build the probe image.")
endif()

set(DS_BOARD_SOURCES "${CMAKE_CURRENT_LIST_DIR}/board.c")
set(DS_ESP32_CHIP "esp32c6")
set(DS_ESP32_CHIP_DIR "${DS_SRC_DIR}/hal/esp32/c6")

set(DS_STARTUP "${DS_SRC_DIR}/startup/startup_esp32.S")

# The contracts of hal/hal.h, split the way the other silicon layers split them:
#   hal_esp32.c      lifecycle, interrupt context, the software reset
#   esp32_irq.c      the interrupt matrix and the PLIC, and the dispatch the trap entry calls
#   esp32_clock.c    the millisecond and the stopwatch on SYSTIMER
#   esp32_wdt.c      the RTC watchdog switched off, MWDT0 armed and fed
#   esp32_panic.c    Panic_Enter, the exception path, and the mark a reset leaves in RAM
#   esp32_nvram.c    the identity: the eFuse MAC, its words read by esp32_ident.c
#   esp32_app_desc.c the descriptor the bootloader reads first
#   esp32_gpio.c     plain outputs, and the board's pins from esp32_board.h in this directory
# and the portable core this silicon's glue calls, compiled here as well as in its host suite:
#   esp32_ident.c    the factory EUI-64 out of the eFuse MAC words
set(DS_HAL_SOURCES
  ${DS_SRC_DIR}/hal/esp32/hal_esp32.c
  ${DS_SRC_DIR}/hal/esp32/esp32_irq.c
  ${DS_SRC_DIR}/hal/esp32/esp32_clock.c
  ${DS_SRC_DIR}/hal/esp32/esp32_wdt.c
  ${DS_SRC_DIR}/hal/esp32/esp32_panic.c
  ${DS_SRC_DIR}/hal/esp32/esp32_nvram.c
  ${DS_SRC_DIR}/hal/esp32/esp32_app_desc.c
  ${DS_SRC_DIR}/hal/esp32/esp32_gpio.c
  ${DS_SRC_DIR}/hal/esp32/esp32_ident.c
)
# The chip's directory before the family's: esp32_chip.h is found there.
set(DS_INCLUDE_DIRS ${DS_ESP32_CHIP_DIR} ${DS_SRC_DIR}/hal/esp32 ${DS_SRC_DIR}/hal/riscv)

# One RAM region holds the IRAM text and the data, as the part's unified SRAM does, so its load segment is
# writable and executable by design.
set(DS_LINK_OPTIONS -Wl,--no-warn-rwx-segments)

# The link is the USB Serial/JTAG controller's CDC port: a byte pipe behind hal/uart_drv.h, so the adapter
# is the silicon-neutral com/uart.c.
set(DS_LINK_SOURCES
  ${DS_SRC_DIR}/hal/esp32/esp32_usj.c
  ${DS_SRC_DIR}/com/uart.c
)

# A bring-up aid, because this board has no debugger attached: the loop reports it is alive, and whether
# the host's octets arrive, every 2 s.
set(DS_LINK_HEARTBEAT_MS "2000u")

# The flash layout: the ota_0 slot of the board's partition table (docs/hardware_esp32.md §12.4).
set(DS_DEFAULT_ORIGIN "0x10000")
set(DS_IMAGE_BOUND "0x210000")
set(DS_LD_FLASH "${DS_ESP32_CHIP_DIR}/esp32c6.ld")
set(DS_GUARD_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/image_guard.cmake")

# The image is an ESP-IDF app, which esptool writes from the ELF: a flat objcopy of a layout with code at
# 0x42000000 and RAM at 0x40800000 would be 32 MB of padding. The flash fields match the board's
# bootloader header (DIO, 80 MHz, 4 MB); the bootloader uses its own, but a reader of the image sees the
# board's.
find_program(DS_ESPTOOL NAMES esptool HINTS ${DS_SRC_DIR}/../.venv-tools/bin)
if(NOT DS_ESPTOOL)
  message(FATAL_ERROR "xiaoc6: esptool not found: python3 -m venv .venv-tools && "
                      ".venv-tools/bin/pip install esptool==5.6.0 (docs/build-and-flash.md §13)")
endif()
set(DS_IMAGE_COMMAND ${DS_ESPTOOL} --chip esp32c6 elf2image --flash-mode dio --flash-freq 80m
    --flash-size 4MB -o ${CMAKE_BINARY_DIR}/${DS_PRODUCT}.bin $<TARGET_FILE:${DS_PRODUCT}>)

# Rf_WakeAndForceClkm, Nvram_SerialAscii, Nvram_Uid and Nvram_ClearUpdateFlag have no counterpart here and
# are not faked: an image that reaches for one fails at the link (docs/boards.md §4).

# The antenna the RF switch selects: the ceramic one on the board, or the U.FL socket (esp32_board.h).
option(DS_ESP32_ANTENNA_UFL "xiaoc6: use the U.FL antenna socket instead of the ceramic antenna" OFF)
if(DS_ESP32_ANTENNA_UFL)
  list(APPEND DS_COMPILE_OPTIONS -DDS_ESP32_ANTENNA_UFL=1)
endif()

# The radio's PHY on Espressif's libraries (hal/esp32/phy/phy.cmake). Until the radio driver lands, a probe
# image built with -DDS_ESP32_PHY_PROBE=ON brings the PHY up at boot and reports how it went.
option(DS_ESP32_PHY_PROBE "xiaoc6: bring the radio's PHY up in the probe image" OFF)
if(DS_ESP32_PHY_PROBE)
  include(${DS_SRC_DIR}/hal/esp32/phy/phy.cmake)
  list(APPEND DS_HAL_SOURCES ${DS_ESP32_PHY_SOURCES})
  list(APPEND DS_COMPILE_OPTIONS -DDS_ESP32_PHY_PROBE=1)
  list(APPEND DS_INCLUDE_DIRS ${DS_SRC_DIR}/hal/esp32/phy)
endif()
