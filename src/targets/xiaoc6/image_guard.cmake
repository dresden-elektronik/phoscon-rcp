# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# The xiaoc6 image guard: the app image esptool wrote must fit the board's ota_0 slot, 0x10000..0x20FFFF,
# which ends where the spiffs partition starts (docs/hardware_esp32.md §12.4).
#
# MAX_BYTES = 0x210000 - 0x10000 = 2097152, computed by the shared build from DS_IMAGE_BOUND and the origin.
# The image's own shape - the descriptor, the two flash segments, the RAM bound - is tools/esp32_check.py's,
# which every flash path runs (docs/build-and-flash.md §13).
file(SIZE ${BIN} _size)
if(NOT _size LESS_EQUAL ${MAX_BYTES})
  message(FATAL_ERROR "${BIN} is ${_size} bytes; the app slot holds ${MAX_BYTES} (0x10000..0x20FFFF)")
endif()
message(STATUS "image guard: ${BIN} = ${_size} bytes (<= ${MAX_BYTES}, the ota_0 slot)")
