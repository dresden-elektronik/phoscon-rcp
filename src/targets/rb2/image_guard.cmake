# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# NVRAM guard: the flat image (origin 0x5000) must end before 0x3EF00,
# where deCONZ NVRAM lives (config sets, MAC, frame counter, EEPROM
# emulation). Writing past it would destroy NVRAM on flash.
#
# MAX_BYTES = 0x3EF00 - 0x5000 = 0x39F00 = 237312.
file(SIZE ${BIN} _size)
if(NOT _size LESS_EQUAL ${MAX_BYTES})
  message(FATAL_ERROR
    "${BIN} is ${_size} bytes; the image must be <= ${MAX_BYTES} bytes "
    "(0x5000..0x3EFFF). Shrink the app or move data out of the image — "
    "NVRAM at 0x3EF00 must never be part of the image.")
endif()
message(STATUS "image guard: ${BIN} = ${_size} bytes (<= ${MAX_BYTES}, ends before 0x3EF00)")
