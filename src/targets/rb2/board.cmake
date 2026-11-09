# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# The RaspBee II target: what the project needs from a board, and nothing else.
#
# The top-level CMakeLists.txt includes targets/${DEBOARD}/board.cmake and consumes the variables
# set here, so adding a board is this file plus board.c, board.h and product.h - no change to the
# shared build (docs/boards.md section 4). The RaspBee II is the same ATSAMR21E18A and the same
# AT86RF233 as the ConBee II; what differs is its host link and the key its GCF container uses
# (docs/hardware_samr21.md §9).

# This board carries MAC security for an OpenThread host, on the transceiver's AES engine, as the
# ConBee II does.
set(DS_THREAD ON)
set(DS_THREAD_AES_ENGINE ON)
set(DS_THREAD_AES_SOURCES ${DS_SRC_DIR}/hal/samr21/samr21_aes.c)
set(DS_THREAD_TX_TIMING ON)
set(DS_CSL_ACCURACY_PPM 40)
set(DS_CSL_UNCERTAINTY_10US 20)
set(DS_BOARD_SOURCES "${CMAKE_CURRENT_LIST_DIR}/board.c")

# The same ATSAMR21E18A as the ConBee II, so the same startup and the same SoC services; only the
# host link differs (docs/boards.md §2).
set(DS_STARTUP "${DS_SRC_DIR}/startup/startup_samr21.S")
set(DS_HAL_SOURCES
  ${DS_SRC_DIR}/hal/samr21/samr21_clock.c
  ${DS_SRC_DIR}/hal/samr21/samr21_rf.c
  ${DS_SRC_DIR}/hal/samr21/samr21_radio.c
  ${DS_SRC_DIR}/hal/samr21/samr21_nvram.c
  ${DS_SRC_DIR}/hal/samr21/samr21_basic.c
  ${DS_SRC_DIR}/hal/samr21/samr21_wdt.c
  ${DS_SRC_DIR}/hal/samr21/samr21_panic.c
  ${DS_SRC_DIR}/hal/samr21/hal_samr21.c
)
set(DS_INCLUDE_DIRS ${DS_SRC_DIR}/hal/samr21 ${DS_SRC_DIR}/hal/cortex_m)

# This board's host link and everything under it: `com/com_uart.c` implements the COM_* contract
# (hal/com.h) over the SERCOM USART driver in hal/samr21/samr21_uart.c. There is no USB device
# stack here: on this board the pad pair PA24/PA25 - the ConBee II's USB DM/DP - is SERCOM5
# PAD2/PAD3 instead, and the link is soldered rather than enumerated.
set(DS_LINK_SOURCES
  ${DS_SRC_DIR}/hal/samr21/samr21_uart.c
  ${DS_SRC_DIR}/com/uart.c
)

# The board's flash layout: where the app is linked and where the deCONZ NVRAM begins. The app
# origin and the NVRAM bound are the same as the ConBee II's - the vendor ships one binary for
# both products, and its containers for this board target 0x5000 too (docs/boards.md §2). What is
# not known is this board's bootloader (docs/hardware_samr21.md §9), so the origin is inherited, not
# verified here.
set(DS_DEFAULT_ORIGIN "0x5000")
set(DS_IMAGE_BOUND "0x3EF00")   # the image ends below the deCONZ NVRAM region
set(DS_LD_FLASH "${CMAKE_CURRENT_LIST_DIR}/atsamr21.ld")
set(DS_GUARD_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/image_guard.cmake")

# The line rate of this board's byte pipe. The rcp image runs at 115200, as a Thread host needs and as
# the ConBee III does; the probe image at 38400, the rate this board's GCF bootloader and the vendor
# flasher speak (docs/hardware_samr21.md §9, docs/build-and-flash.md §5.1).
if(DEFINED DS_MODE AND DS_MODE STREQUAL "rcp")
  set(DS_LINK_BAUD "115200u" CACHE STRING "rb2 host-link line rate, e.g. 115200u")
else()
  set(DS_LINK_BAUD "38400u" CACHE STRING "rb2 host-link line rate, e.g. 38400u")
endif()

# The link never disconnects, so a flasher waits for the reply to its reset frame (app/flasher_trap.h).
set(DS_FLASHER_ACK ON)

# This board provides the diagnostics hal/hal_diag.h declares: the bring-up stopwatch, the clock-tree
# register witnesses and the post-mortem scratch row (docs/architecture.md §2). The shared app layer
# calls those under DS_BOARD_DIAG, so a board whose silicon has none of them - an MG21 has no 0x3E000
# row and no GCLK tree to witness - is not asked to invent them (docs/boards.md §4).
set(DS_BOARD_DIAG ON)
