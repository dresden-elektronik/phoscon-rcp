# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# The ConBee I / RaspBee I target: one image for both products (docs/hardware_avr.md §11.8), on the ATmega256RFR2's
# own silicon layer, src/hal/avr/, which shares no source with hal/samr21/ (docs/architecture.md §2).
#
# Configure with -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/avr-gcc.cmake: an 8-bit core needs its own toolchain
# file. Both images build: the probe image boots, runs the CPU at 16 MHz, keeps the millisecond and the
# watchdog and publishes its records on USART0 at 38 400; the rcp image adds the transceiver driver
# behind hal/radio.h (hal/avr/avr_radio.c) and speaks Spinel on the same link at 57 600 (docs/hardware_avr.md §11.8).
set(DS_BOARD_SOURCES "${CMAKE_CURRENT_LIST_DIR}/board.c")

set(DS_STARTUP "${DS_SRC_DIR}/startup/startup_avr.S")

# The probe image links at 38 400, the rate the bootloader, the vendor firmware and the vendor flasher speak (§11.8).
#
# The rcp image serves a Zigbee and a Thread host alike (docs/hardware_avr.md §11.7): MAC security on the transceiver's
# AES engine, timed transmit, and 57 600, the slowest rate the Home Assistant app offers and the one the USART reaches
# inside its error with the radio vector's masked stretches. DS_MODE is declared further down, so this reads the value
# the command line gave.
if(DEFINED DS_MODE AND DS_MODE STREQUAL "rcp")
  set(DS_THREAD ON)
  set(DS_THREAD_AES_ENGINE ON)
  set(DS_THREAD_AES_SOURCES ${DS_SRC_DIR}/hal/avr/avr_aes.c)
  set(DS_THREAD_TX_TIMING ON)
  set(DS_CSL_ACCURACY_PPM 40)
  set(DS_CSL_UNCERTAINTY_10US 20)
  set(DS_RADIO_WITNESS ON)
  set(DS_RCP_RADIO_SOURCE ${DS_SRC_DIR}/hal/avr/avr_radio.c)
  set(DS_LINK_BAUD "57600u" CACHE STRING "cbrb1 host-link line rate, e.g. 57600u")
else()
  set(DS_LINK_BAUD "38400u" CACHE STRING "cbrb1 host-link line rate, e.g. 38400u")
endif()

# The contracts of hal/hal.h, split the way the other silicon layers split them:
#   hal_avr.c    lifecycle, interrupt context, and the watchdog reset into the bootloader
#   avr_clock.c  the prescaler, the millisecond on Timer/Counter0, the stopwatch on Timer/Counter1
#   avr_wdt.c    arming, feeding, and the reset period
#   avr_panic.c  Panic_Enter, the unclaimed vectors, and the mark a reset leaves in SRAM
#   avr_nvram.c  the identity: the NV basic set in the EEPROM, its bytes read by avr_ident.c
#   avr_radio.c  the on-die transceiver behind hal/radio.h, in the rcp image only
# and the two portable cores this silicon's glue calls, compiled here as well as in their host suites:
#   avr_ident.c  the factory EUI-64 out of the NV basic set
#   avr_baud.c   the USART divisor
set(DS_HAL_SOURCES
  ${DS_SRC_DIR}/hal/avr/hal_avr.c
  ${DS_SRC_DIR}/hal/avr/avr_clock.c
  ${DS_SRC_DIR}/hal/avr/avr_wdt.c
  ${DS_SRC_DIR}/hal/avr/avr_panic.c
  ${DS_SRC_DIR}/hal/avr/avr_nvram.c
  ${DS_RCP_RADIO_SOURCE}
  ${DS_SRC_DIR}/hal/avr/avr_ident.c
  ${DS_SRC_DIR}/hal/avr/avr_baud.c
)
set(DS_INCLUDE_DIRS ${DS_SRC_DIR}/hal/avr)

# The link is a USART, so the adapter is the silicon-neutral com/uart.c over this board's driver.
set(DS_LINK_SOURCES
  ${DS_SRC_DIR}/hal/avr/avr_uart.c
  ${DS_SRC_DIR}/com/uart.c
)

# The flash layout: the application from 0 to the bootloader's boot section (docs/hardware_avr.md §11.1).
# Origin 0 is the real layout here, so both scripts are the same one.
set(DS_DEFAULT_ORIGIN "0x0")
set(DS_IMAGE_BOUND "0x3E000")
set(DS_LD_FLASH "${CMAKE_CURRENT_LIST_DIR}/atmega256rfr2.ld")
set(DS_GUARD_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/image_guard.cmake")

# GCFFlasher sends its reset frame and waits for the vendor firmware's reply, or for the device to
# disconnect, before it looks for the bootloader; an FTDI port never disconnects and the bootloader's
# window is half a second, so this image replies (app/flasher_trap.h, docs/build-and-flash.md §11).
set(DS_FLASHER_ACK ON)

# Rf_WakeAndForceClkm, Nvram_SerialAscii, Nvram_Uid and Nvram_ClearUpdateFlag have no counterpart here
# and are not faked: an image that reaches for one fails at the link. DS_BOARD_DIAG is not set - the
# diagnostics hal/hal_diag.h declares are the SAMR21's; this board reports Clock_WitnessReport and
# Uart_Witnesses from its own files.
