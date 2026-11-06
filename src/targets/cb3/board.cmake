# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# The ConBee III target: the board's data, and the silicon layer that fills its contracts.
#
# The shared build consumes DS_BOARD_SOURCES, DS_STARTUP, DS_HAL_SOURCES, DS_INCLUDE_DIRS,
# DS_LINK_SOURCES, the linker scripts and DS_IMAGE_BOUND from this file (docs/boards.md §4). The layout
# is in flash_layout.cmake - the bootloader's formula over the part's own numbers, asserted against the
# regions docs/hardware_mg21.md §10.7 records, and cross-checked against the linker scripts.
#
# What this board builds today: it boots on the Cortex-M33, brings the clocks and the watchdog up, opens
# USART0 as the host link, answers Spinel, and in the rcp image receives and transmits 802.15.4
# frames on Silicon Labs' RAIL (hal/mg21/rail/, docs/hardware_mg21.md §10.6). This tree's own back-end
# (hal/mg21/own/, docs/hardware_mg21.md §10.5) stays in the tree and is built only with
# -DDS_MG21_RADIO=own; docs/hardware_mg21.md §10.5 says what it does not do yet. The probe image drives no
# radio, so it links neither.
#
# The probe image runs on the bench ConBee III and GCFFlasher updates it over the link
# (docs/build-and-flash.md §9), and the rcp image passes tools/cb3_rcp_check.py on the bench.
include(${CMAKE_CURRENT_LIST_DIR}/flash_layout.cmake)

# DS_MODE is declared by the shared build after this file, so this reads the value the command line gave.
if(DEFINED DS_MODE AND DS_MODE STREQUAL "rcp")
  set(CB3_BACK_END ${DS_MG21_RADIO})
else()
  set(CB3_BACK_END "none")
endif()

# On the RAIL back-end this board carries what an OpenThread host requires. The own back-end sends no
# Ack, so it is not a Thread radio. Accuracy is 802.15.4's own limit for the crystal, 40 ppm.
if(CB3_BACK_END STREQUAL "rail")
  set(DS_THREAD ON)
  set(DS_THREAD_TX_TIMING ON)
  set(DS_THREAD_AES_ENGINE ON)
  set(DS_CSL_ACCURACY_PPM 40)
  set(DS_CSL_UNCERTAINTY_10US 20)
endif()
set(DS_BOARD_SOURCES "${CMAKE_CURRENT_LIST_DIR}/board.c")

# The bound is CB3_IMAGE_BOUND: the end of the AppSection minus the 16-byte AppInfo trailer the
# bootloader writes after the app. flash_layout.cmake derives it, asserts it against the documented
# layout, and checks that both linker scripts end their FLASH region at the same address, so the number
# the app budget uses (491 504 B), the number the linker enforces and the number
# tools/gcf_check.py / GCFTool's cb3 profile apply to the container cannot drift apart.
set(DS_DEFAULT_ORIGIN "0x4000")
set(DS_IMAGE_BOUND "0x7BFF0")
set(DS_LD_FLASH "${CMAKE_CURRENT_LIST_DIR}/efr32mg21.ld")
set(DS_GUARD_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/image_guard.cmake")

# The line rate of this board's byte pipe: 115 200, which is what the DeviceConfig CommConfig in the
# user data page carries and what the bootloader, the vendor firmware and the vendor flasher all use
# (docs/hardware_mg21.md §10.8).
set(DS_LINK_BAUD "115200u" CACHE STRING "cb3 host-link line rate, e.g. 115200u")

# A bring-up aid, because this board has no debugger: the heartbeat reports the live state of the
# receive path every 2 s (hal/mg21/mg21_uart.c), which is how a link that transmits but does not
# receive is told apart from one that receives nothing (docs/hardware_mg21.md §10.8).
set(DS_LINK_HEARTBEAT_MS "2000u")

# The bench register monitor, the instrument for a radio nothing else is driving: the probe image takes
# read, write and trace commands on its link, inside the region table in hal/mg21/mg21_mon.c, which
# never lets a write reach MSC, SMU or the Secure Engine mailbox.
set(DS_MONITOR ON)
set(DS_MONITOR_SOURCES "${DS_SRC_DIR}/hal/mg21/mg21_mon.c")

# A Cortex-M33 needs its own toolchain file, and it is not this directory's to choose: configure with
# -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi-m33.cmake (docs/boards.md §4). The startup file
# declares .cpu cortex-m33 and test/unit/test_mg21_vectors.py checks that the table it lays out is the
# part's, slot for slot.
set(CB3_STARTUP "${DS_SRC_DIR}/startup/startup_mg21.S")

# The radio back-end is a configure-time choice (docs/build-and-flash.md §10): "rail" is the one on
# Silicon Labs' RAIL library of §10.6, "own" this tree's from-scratch route of docs/hardware_mg21.md §10.5.
# The cache variable itself is declared by the shared build, which also refuses it on any other board.
if(CB3_BACK_END STREQUAL "rail")
  include(${CMAKE_CURRENT_LIST_DIR}/rail.cmake)
  set(CB3_RADIO ${DS_RAIL_SOURCES})
elseif(CB3_BACK_END STREQUAL "own")
  set(CB3_RADIO
    "${DS_SRC_DIR}/hal/mg21/own/mg21_radio.c"
    "${DS_SRC_DIR}/hal/mg21/own/mg21_seq_image.c"
    "${DS_SRC_DIR}/hal/mg21/own/mg21_radio_cfg.c"
    "${DS_SRC_DIR}/hal/mg21/own/mg21_phy.c"
  )
else()
  set(CB3_RADIO "${DS_SRC_DIR}/hal/mg21/mg21_radio_none.c")
endif()

# The contracts of hal/hal.h and hal/radio.h, split the way the SAMR21 splits them:
#   hal_mg21.c       lifecycle, interrupt context, and the reset that asks the bootloader to stay
#   mg21_clock.c     Clock_*, Tick_*, Delay_Ms, the cycle stopwatch, the clock read-backs
#   mg21_wdt.c       Wdt_Feed, and arming the watchdog to a period the loop can feed
#   mg21_panic.c     Panic_Enter, the fault vectors, and the mark a reset leaves in SRAM
#   mg21_nvram.c     the identity: the factory EUI-64 from the user data page (docs/hardware_mg21.md
#                    §10.2), its bytes read by mg21_ident.c
#   own/             the "own" radio back-end: mg21_radio.c (install, configure, receive,
#                    docs/hardware_mg21.md §10.5), the sequencer program it installs (mg21_seq_image.c,
#                    from tools/mg21_seq.py), its register tables (mg21_radio_cfg.c, generated by
#                    tools/mg21_radio_gen.py), and mg21_phy.c, the channel arithmetic and receive-record
#                    format, a portable core under its host suite
#   rail/            the "rail" radio back-end in their place: the adapter, its power curves and the
#                    symbols the vendor archive expects (docs/hardware_mg21.md §10.6)
#   mg21_radio_none.c what the probe image has instead of a back-end: only the hand-back
#   mg21_radio_off.c the hand-back of the radio before a reset, shared by both back-ends
#   tick_plan.c      the millisecond arithmetic, pure and host-tested like usart_baud.c
# and the two portable cores this silicon's glue calls, compiled here as well as in their host
# suites so the freestanding rules are enforced on the target too:
#   usart_baud.c          the Series 2 line-rate divisor
#   mg21_reset_record.c   the boot record's rules, with the address supplied by hal_mg21.c
#   mg21_ident.c          the factory EUI-64 out of its token, with the bytes supplied by mg21_nvram.c
set(CB3_HAL
  "${DS_SRC_DIR}/hal/mg21/hal_mg21.c"
  "${DS_SRC_DIR}/hal/mg21/mg21_clock.c"
  "${DS_SRC_DIR}/hal/mg21/mg21_wdt.c"
  "${DS_SRC_DIR}/hal/mg21/mg21_panic.c"
  "${DS_SRC_DIR}/hal/mg21/mg21_nvram.c"
  ${CB3_RADIO}
  "${DS_SRC_DIR}/hal/mg21/mg21_radio_off.c"
  "${DS_SRC_DIR}/hal/mg21/tick_plan.c"
  "${DS_SRC_DIR}/hal/mg21/usart_baud.c"
  "${DS_SRC_DIR}/hal/mg21/mg21_reset_record.c"
  "${DS_SRC_DIR}/hal/mg21/mg21_ident.c"
)

# The link is a USART, so the adapter is the silicon-neutral com/uart.c over this board's driver, which
# fills hal/uart_drv.h (docs/boards.md §2).
set(CB3_LINK
  "${DS_SRC_DIR}/hal/mg21/mg21_uart.c"
  "${DS_SRC_DIR}/com/uart.c"
)

# Three hal/hal.h entries have no ConBee III counterpart and this board does not fake them:
# Rf_WakeAndForceClkm (the RF233 is a separate chip woken over SPI; the MG21's radio is on-die),
# Nvram_ClearUpdateFlag (the SAMR21 bootloader's flag at 0x4FFF; this bootloader verifies with the
# AppInfo trailer), and Nvram_Uid plus Nvram_SerialAscii (USB descriptor entries, and this board links
# no USB stack). None is referenced by a cb3 image, so the honest outcome is that they are absent, and
# a build that ever reaches for one fails at the link rather than reading a lie (docs/boards.md §4).
#
# DS_BOARD_DIAG is deliberately not set: hal/hal_diag.h declares the SAMR21's bring-up stopwatch,
# clock-tree witnesses and post-mortem scratch row, and this silicon has none of those. What this board
# reports instead is its own read-backs - Clock_WitnessReport and the driver's Uart_Witnesses - emitted
# from the board's files, and the shared loop calls neither.
set(CB3_MISSING "")
foreach(_f ${DS_BOARD_SOURCES} ${CB3_STARTUP} ${CB3_HAL} ${CB3_LINK})
  if(NOT EXISTS ${_f})
    string(APPEND CB3_MISSING "  ${_f}\n")
  endif()
endforeach()

if(CB3_MISSING)
  message(FATAL_ERROR
    "cb3: the board's layout is here (flash_layout.cmake, the linker scripts and the guard in this "
    "directory) and its silicon layer is not complete. These files are the board's contracts "
    "(board facts in docs/hardware_mg21.md §10.8):\n${CB3_MISSING}The two "
    "SAMR21 targets are unaffected by this board.")
endif()

set(DS_STARTUP ${CB3_STARTUP})
set(DS_HAL_SOURCES ${CB3_HAL})
set(DS_INCLUDE_DIRS ${DS_SRC_DIR}/hal/mg21)
set(DS_LINK_SOURCES ${CB3_LINK})
