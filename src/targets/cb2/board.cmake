# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# The ConBee II target: what the project needs from a board, and nothing else.
#
# The top-level CMakeLists.txt includes targets/${DEBOARD}/board.cmake and consumes the variables set
# here, so adding a board is this file plus board.c, board.h and product.h - no change to the shared
# build. See docs/boards.md section 4 for the checklist, including what a board with a different core
# needs beyond this file.
# This board carries MAC security for an OpenThread host, on the transceiver's AES engine.
set(DS_THREAD ON)
set(DS_THREAD_AES_ENGINE ON)
set(DS_THREAD_AES_SOURCES ${DS_SRC_DIR}/hal/samr21/samr21_aes.c)
set(DS_THREAD_TX_TIMING ON)
set(DS_CSL_ACCURACY_PPM 40)
set(DS_CSL_UNCERTAINTY_10US 20)
set(DS_BOARD_SOURCES "${CMAKE_CURRENT_LIST_DIR}/board.c")

# The silicon this board is: the ATSAMR21E18A startup code and the SoC services behind hal/hal.h.
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

# This board's host link and everything under it. The link is the ConBee II's native USB CDC:
# `com/usb_cdc.c` implements the COM_* contract (hal/com.h) over the USB device stack in usb/ -
# the driver adopted from our own released firmware (deconz-cb2-fw .../USBClasses/cdc), its
# descriptor content ours, plus the SAMR21 register bridge and the USB device API Usbc_*. Naming
# exception documented in usb/usbd.h. A board whose link is a UART lists a SERCOM driver here
# instead (docs/boards.md §4).
set(DS_LINK_SOURCES
  ${DS_SRC_DIR}/usb/usb_desc.c
  ${DS_SRC_DIR}/usb/usbd_cdc.c
  ${DS_SRC_DIR}/usb/usbd_core.c
  ${DS_SRC_DIR}/usb/usbd_ep0.c
  ${DS_SRC_DIR}/usb/usbd_req.c
  ${DS_SRC_DIR}/hal/samr21/samr21_usb_device.c
  ${DS_SRC_DIR}/hal/samr21/samr21_usbd_hal.c
  ${DS_SRC_DIR}/com/usb_cdc.c
)

# The board's flash layout: where the app is linked and where the deCONZ NVRAM begins. Both are board
# facts (docs/boards.md section 4) - 0x5000 is behind this board's GCF bootloader, and the image must end
# before the NVRAM region, which has to survive every flash.
set(DS_DEFAULT_ORIGIN "0x5000")
set(DS_IMAGE_BOUND "0x3EF00")   # the image ends below the deCONZ NVRAM region
set(DS_LD_FLASH "${CMAKE_CURRENT_LIST_DIR}/atsamr21.ld")
set(DS_GUARD_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/image_guard.cmake")

# This board provides the diagnostics hal/hal_diag.h declares: the bring-up stopwatch, the clock-tree
# register witnesses and the post-mortem scratch row (docs/architecture.md §2). The shared app layer
# calls those under DS_BOARD_DIAG, so a board whose silicon has none of them - an MG21 has no 0x3E000
# row and no GCLK tree to witness - is not asked to invent them (docs/boards.md §4).
set(DS_BOARD_DIAG ON)
