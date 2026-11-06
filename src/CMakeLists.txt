# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# Firmware: the Spinel RCP. The board is selected with -DDEBOARD (docs/boards.md).
# Configure with: cmake -B build/fw -S src -G Ninja \
#   -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake   (absolute: a relative path is resolved
#   against the build directory, which silently leaves the compiler unset)
cmake_minimum_required(VERSION 3.16)

# The product name, in one place: it is the CMake target, the artifact in the build tree
# and the name the flash pipeline in AGENTS.md uses: the project's, phoscon-rcp, for every board.
# The firmware's identity on the wire comes from the board's product.h ("DSPINEL/1.0; AT86RF233; ..."
# for the ConBee II), whose prefix hosts key on and which does not change with the project name.
# Which board this build is for. The board owns its descriptor, its linker scripts and its image
# guard; everything else in this file is shared (docs/boards.md section 4). An unknown board fails here
# rather than half-configuring, and the old variable names fail loudly too: a build directory configured
# before the rename would otherwise silently fall back to the default mode and produce the wrong image.
set(DEBOARD "cb2" CACHE STRING "board target: cb2, rb2, cb3 (cb3's radio back-end receives and transmits, docs/hardware_mg21.md §10.5)")
set(DEBOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/targets/${DEBOARD}")
if(NOT EXISTS "${DEBOARD_DIR}/board.cmake")
  message(FATAL_ERROR "no board '${DEBOARD}': ${DEBOARD_DIR}/board.cmake is missing. A board is that "
                      "file plus board.c, board.h and product.h - see docs/boards.md section 4.")
endif()

# The ConBee III's radio back-end (docs/build-and-flash.md §10): Silicon Labs' RAIL library fetched at
# configure time, the default, or this tree's own route, chosen only by asking for it. Only the rcp image
# drives the radio, so only it links a back-end (targets/cb3/board.cmake). A choice that only one board
# has is refused on every other.
set(DS_MG21_RADIO "" CACHE STRING "cb3 radio back-end: rail | own; empty = rail (docs/build-and-flash.md §10)")
set_property(CACHE DS_MG21_RADIO PROPERTY STRINGS "" rail own)
if(DS_MG21_RADIO STREQUAL "")
  if(DEBOARD STREQUAL "cb3")
    set(DS_MG21_RADIO "rail")
  else()
    set(DS_MG21_RADIO "own")
  endif()
endif()
if(NOT DS_MG21_RADIO STREQUAL "own" AND NOT DS_MG21_RADIO STREQUAL "rail")
  message(FATAL_ERROR "DS_MG21_RADIO='${DS_MG21_RADIO}': the radio back-end is 'own' or 'rail'")
endif()
if(DS_MG21_RADIO STREQUAL "rail" AND NOT DEBOARD STREQUAL "cb3")
  message(FATAL_ERROR "DS_MG21_RADIO=rail is the ConBee III's option (the EFR32MG21 radio, Silicon Labs' RAIL "
                      "library); board '${DEBOARD}' has no such back-end. Drop -DDS_MG21_RADIO or use -DDEBOARD=cb3.")
endif()

if(DEFINED CB2_MODE OR DEFINED CB2_PRODUCT OR DEFINED CB2_LD)
  message(FATAL_ERROR "this build directory was configured with the old CB2_* variable names; delete it "
                      "and configure fresh (see docs/boards.md section 4)")
endif()

set(DS_PRODUCT "phoscon-rcp")

# The firmware source root, handed to the board so its own source lists do not have to guess the
# distance from their directory.
set(DS_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

include(${DEBOARD_DIR}/board.cmake)

# Every board has a host link, and what sits under that link is board-specific: a CDC link needs a
# USB device stack, a UART link needs a SERCOM driver. The board names those files.
if(NOT DEFINED DS_LINK_SOURCES OR DS_LINK_SOURCES STREQUAL "")
  message(FATAL_ERROR "no board '${DEBOARD}': ${DEBOARD_DIR}/board.cmake must set DS_LINK_SOURCES to the "
                      "COM_* implementation of this board's link and the sources under it "
                      "(see docs/boards.md section 4).")
endif()

# The silicon too is the board's decision: the startup code and the SoC services behind hal/hal.h.
# This file used to name them literally, which silently tied the shared build to the SAMR21 - an MG21
# board cannot compile startup_samr21.S (docs/boards.md §4).
if(NOT DEFINED DS_HAL_SOURCES OR DS_HAL_SOURCES STREQUAL "")
  message(FATAL_ERROR "no board '${DEBOARD}': ${DEBOARD_DIR}/board.cmake must set DS_HAL_SOURCES to the "
                      "HAL_* implementation for this board's SoC, and DS_STARTUP to its startup code.")
endif()

project(${DS_PRODUCT} C ASM)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Where the app image is linked: the board's origin, behind its bootloader (0x5000 on the SAMR21
# boards, 0x4000 on the ConBee III, 0x0 on the AVR boards, whose bootloader sits at the top of flash).
set(APP_ORIGIN "${DS_DEFAULT_ORIGIN}")

add_executable(${DS_PRODUCT}
  # Startup and the SoC services behind hal/hal.h: both named by the board, because both are silicon
  # facts (docs/boards.md §4).
  ${DS_STARTUP}
  ${DS_HAL_SOURCES}
  # Shared, target-agnostic glue: the debug-record queue and the line-rate arithmetic.
  hal/dbg.c
  hal/uart_baud.c
  # The host link and everything under it: the COM_* contract (hal/com.h), its implementation for
  # this board, and the device stack it sits on. Which files those are is the board's decision
  # (docs/boards.md §3/§4) - the ConBee II's link is native USB CDC over the adopted USB device
  # stack, the RaspBee II's is a SERCOM5 USART. The list arrives as DS_LINK_SOURCES.
  ${DS_LINK_SOURCES}
  # The board descriptor behind the contract in board/board.h: one instance per
  # board, named by ${DEBOARD} (targets/${DEBOARD}/board.c).
  ${DS_BOARD_SOURCES}
  # RCP core (docs/architecture.md). Pure C, no hardware - compiled here as
  # well as in the host suite so the freestanding rules (no libc, no dynamic
  # allocation) are enforced on the target too. Unreferenced for now, so
  # gc-sections drops it and the shipped image is unchanged until P3 wires the
  # Spinel session to the USB transport.
  rcp/hdlc.c
  rcp/spinel.c
  rcp/props.c
  rcp/mac.c
  rcp/scan.c
  rcp/session.c
  rcp/props_stream.c
  rcp/props_debug.c
  rcp/srcmatch.c
  app/main.c
  app/flasher_trap.c
  app/rcp_app.c
  lib/u_bstream/u_bstream.c
  lib/u_sstream/u_sstream.c
)

# The version an rcp image reports in NCP_VERSION (cmake/version.cmake, docs/build-and-flash.md §12). It
# runs on every build, so an image cannot name a commit it was not built from.
set(DS_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/phoscon_version.h")
add_custom_target(${DS_PRODUCT}_version
  COMMAND ${CMAKE_COMMAND} -DDS_REPO_DIR=${DS_SRC_DIR}/.. -DDS_VERSION_HEADER=${DS_VERSION_HEADER}
          -P ${DS_SRC_DIR}/../cmake/version.cmake
  BYPRODUCTS ${DS_VERSION_HEADER}
  VERBATIM)
add_dependencies(${DS_PRODUCT} ${DS_PRODUCT}_version)
target_include_directories(${DS_PRODUCT} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

target_include_directories(${DS_PRODUCT} PRIVATE
  app
  rcp
  board
  hal
  usb
  com
  lib/u_bstream
  lib/u_sstream
  ${DEBOARD_DIR}
  # The directories the board's own sources need (hal/samr21 today, hal/mg21 for a MG21 board).
  ${DS_INCLUDE_DIRS}
)

# A board whose radio is a vendor library names it here (cb3 with DS_MG21_RADIO=rail, targets/cb3/rail.cmake):
# the options every translation unit and the link must share with it, and the archive, which has to come
# before libgcc because it calls into it.
if(DS_COMPILE_OPTIONS)
  target_compile_options(${DS_PRODUCT} PRIVATE ${DS_COMPILE_OPTIONS})
endif()
if(DS_LINK_OPTIONS)
  target_link_options(${DS_PRODUCT} PRIVATE ${DS_LINK_OPTIONS})
endif()
if(DS_LINK_LIBRARIES)
  target_link_libraries(${DS_PRODUCT} PRIVATE ${DS_LINK_LIBRARIES})
endif()

# libgcc (jump-table helpers) must come after the objects: -nostdlib
# does not pull it in.
target_link_libraries(${DS_PRODUCT} PRIVATE gcc)

# Which board's diagnostics the shared loop may call: hal/hal_diag.h declares the SAMR21's bring-up
# stopwatch, clock-tree witnesses and post-mortem scratch row, and only a board that has them says so
# here (docs/boards.md §4). A board that does not is not asked to invent them.
if(DS_BOARD_DIAG)
  target_compile_definitions(${DS_PRODUCT} PRIVATE DS_BOARD_DIAG=1)
endif()

# A board with no such diagnostics can still publish its radio vector's statistics.
if(DS_RADIO_WITNESS)
  target_compile_definitions(${DS_PRODUCT} PRIVATE DS_RADIO_WITNESS=1)
endif()

# App flash origin for code (VTOR, origin-relative assumptions). APP_ORIGIN itself is
# declared near the top of this file, before this first use - it used to be defaulted
# *here*, seven lines too late, which is why a fresh configure built with
# `APP_FLASH_ORIGIN=U` (see the comment there).
target_compile_definitions(${DS_PRODUCT} PRIVATE APP_FLASH_ORIGIN=${APP_ORIGIN}U)

# A link with a line rate of its own gets it from the board, as a compile-time definition the
# board's product.h can lean on. A CDC link defines nothing: its rate is the bus's.
if(DEFINED DS_LINK_BAUD)
  target_compile_definitions(${DS_PRODUCT} PRIVATE PRODUCT_LINK_BAUD=${DS_LINK_BAUD})
endif()

# A bring-up aid a board may ask for (docs/boards.md §4): a live link report every DS_LINK_HEARTBEAT_MS.
# A board that does not set it builds exactly as before.
if(DEFINED DS_LINK_HEARTBEAT_MS)
  target_compile_definitions(${DS_PRODUCT} PRIVATE DS_LINK_HEARTBEAT_MS=${DS_LINK_HEARTBEAT_MS})
endif()

# A board whose host flasher waits for the reply to its reset frame before it looks for the bootloader
# (app/flasher_trap.h: the ConBee I and RaspBee I, behind a link that never disconnects) sets
# DS_FLASHER_ACK, and the loop sends the vendor firmware's reply. A board that does not builds exactly
# as before.
if(DS_FLASHER_ACK)
  target_compile_definitions(${DS_PRODUCT} PRIVATE DS_FLASHER_ACK=1)
endif()

# The bench register monitor (docs/hardware_mg21.md §10.5): a probe image of a board that sets
# DS_MONITOR takes read, write and trace commands on its link. The board names its access glue in
# DS_MONITOR_SOURCES (hal/mon_drv.h); the command format and the address policy are shared core.
if(DS_MONITOR)
  if(NOT DEFINED DS_MONITOR_SOURCES OR DS_MONITOR_SOURCES STREQUAL "")
    message(FATAL_ERROR "board '${DEBOARD}' sets DS_MONITOR but names no DS_MONITOR_SOURCES (hal/mon_drv.h)")
  endif()
  target_sources(${DS_PRODUCT} PRIVATE app/mon_proto.c app/monitor_app.c ${DS_MONITOR_SOURCES})
  target_compile_definitions(${DS_PRODUCT} PRIVATE DS_MONITOR=1)
endif()

# The board's linker script.
set(DS_LD "${DS_LD_FLASH}")
set_target_properties(${DS_PRODUCT} PROPERTIES
  LINK_FLAGS "-T ${DS_LD} -Wl,-Map=${CMAKE_BINARY_DIR}/${DS_PRODUCT}.map"
)

# Flat binary (objcopy) + the board's image-size guard: the image must end below DS_IMAGE_BOUND.
# On a SAMR21 board that bound is the deCONZ NVRAM region, which has to survive every reflash; on the
# ConBee III it is the end of the bootloader's app sector. MAX_BYTES is derived from the origin, never
# listed.
if(NOT DEFINED DS_IMAGE_BOUND OR DS_IMAGE_BOUND STREQUAL "")
  message(FATAL_ERROR "no board '${DEBOARD}': ${DEBOARD_DIR}/board.cmake must set DS_IMAGE_BOUND, the "
                      "address the image must end below (docs/boards.md §4).")
endif()
math(EXPR DS_GUARD_MAX "${DS_IMAGE_BOUND} - ${APP_ORIGIN}")
add_custom_command(TARGET ${DS_PRODUCT} POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${DS_PRODUCT}>
          ${CMAKE_BINARY_DIR}/${DS_PRODUCT}.bin
  COMMAND ${CMAKE_COMMAND}
          -D MAX_BYTES=${DS_GUARD_MAX}
          -D BIN=${CMAKE_BINARY_DIR}/${DS_PRODUCT}.bin
          -P ${DS_GUARD_SCRIPT}
  COMMENT "objcopy -O binary + NVRAM guard"
)


# Application mode (docs/architecture.md §1): "probe" is the diagnostic and
# shipping default - CDC IN carries debug records, CDC OUT is the flasher trap. "rcp"
# makes the same endpoints carry Spinel instead: the records are still produced and
# queued but not published, because the transport owns the IN endpoint. Both modes keep
# the reset trap, and neither needs a descriptor change - the D1 two-interface design is
# a later step.
set(DS_MODE "probe" CACHE STRING "app mode: probe | rcp")
if(DS_MODE STREQUAL "rcp")
  set(DS_APP_MAIN app/main_rcp.c)
else()
  set(DS_APP_MAIN app/main_probe.c)
endif()
target_sources(${DS_PRODUCT} PRIVATE ${DS_APP_MAIN})

# The post-mortem scratch row is the probe image's. Rewriting it stalls every flash fetch, vectors
# included, and wears the row, so an rcp image never writes it (hal/hal_diag.h).
if(DS_BOARD_DIAG AND NOT DS_MODE STREQUAL "rcp")
  target_compile_definitions(${DS_PRODUCT} PRIVATE DS_SCRATCH_ROW=1)
endif()

# Thread support, per board (board.cmake sets DS_THREAD): MAC security for an OpenThread host.
# What a board without it advertises and links stays as it was.
if(DS_THREAD AND DS_MODE STREQUAL "rcp")
  # The block cipher is a seam (lib/aes/aes.h): a board with an engine brings its own, in DS_HAL_SOURCES.
  target_sources(${DS_PRODUCT} PRIVATE rcp/mac_sec.c lib/aes/aes_modes.c)
  if(NOT DS_THREAD_AES_ENGINE)
    target_sources(${DS_PRODUCT} PRIVATE lib/aes/aes_table.c)
  elseif(DS_THREAD_AES_SOURCES)
    target_sources(${DS_PRODUCT} PRIVATE ${DS_THREAD_AES_SOURCES})
  endif()
  target_include_directories(${DS_PRODUCT} PRIVATE lib/aes)
  target_compile_definitions(${DS_PRODUCT} PRIVATE RCP_THREAD=1)
  # Timed transmit, for a board whose back-end has it, and what a CSL child is told to allow for.
  if(DS_THREAD_TX_TIMING)
    target_compile_definitions(${DS_PRODUCT} PRIVATE RCP_TX_TIMING=1
      RCP_CSL_ACCURACY_PPM=${DS_CSL_ACCURACY_PPM} RCP_CSL_UNCERTAINTY_10US=${DS_CSL_UNCERTAINTY_10US})
  endif()
  # GCC turns their byte loops into memcpy and memset, which a -nostdlib image does not have
  # (docs/coding-rules.md), and the vendored files are not restyled to prevent it.
  set_source_files_properties(rcp/mac_sec.c lib/aes/aes_modes.c lib/aes/aes_table.c
    hal/samr21/samr21_radio.c hal/samr21/samr21_aes.c hal/avr/avr_radio.c hal/avr/avr_aes.c PROPERTIES
    COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")
endif()

# Main-loop instrumentation (app/loop_diag.c, loop_diag.h). Off in the shipped image; a
# diagnostic build passes -DDS_LOOP_DIAG=ON and the loop then publishes its iterations per
# second, a record for any phase slower than 20 ms, and the NVRAM row-patch rate. The
# records use the existing queue/transport, so nothing else changes.
option(DS_LOOP_DIAG "main-loop instrumentation (loop_diag.c)" OFF)
if(DS_LOOP_DIAG)
  target_sources(${DS_PRODUCT} PRIVATE app/loop_diag.c)
  target_compile_definitions(${DS_PRODUCT} PRIVATE DS_LOOP_DIAG=1)
endif()
