# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# AppSection guard: the flat image (origin 0x4000) must end before 0x7BFF0, where the deSiLabs
# bootloader writes its 16-byte AppInfo trailer after the app.
#
# MAX_BYTES = 0x7BFF0 - 0x4000 = 0x77FF0 = 491504, computed by the shared build from DS_IMAGE_BOUND and
# the origin. The bootloader's own check
# is laxer - de_BTL_flash_installImage accepts a plain size up to the whole 491520-byte AppSection - and
# an image that fills it is written, then fails de_BTL_flash_verifyApp against the trailer that overwrote
# its last 16 bytes. The board would sit in DFU holding an image the host believed in, which is why the
# stricter number is enforced here and in tools/gcf_check.py (docs/hardware_mg21.md §10.7).
#
# Nothing below 0x4000 is ever part of this image: that is the BTL sector. NVM3 at 0xF8000 and the
# user data page (EUI-64, serial, CTUNE, DeviceConfig) are outside both regions and untouched.
file(SIZE ${BIN} _size)
if(NOT _size LESS_EQUAL ${MAX_BYTES})
  message(FATAL_ERROR
    "${BIN} is ${_size} bytes; the image must be <= ${MAX_BYTES} bytes (0x4000..0x7BEFF). Shrink the "
    "app: the last 16 bytes of the AppSection belong to the bootloader's AppInfo trailer, and an image "
    "that reaches it is rejected by the bootloader after it has already been written.")
endif()
message(STATUS "image guard: ${BIN} = ${_size} bytes (<= ${MAX_BYTES}, ends before the AppInfo trailer at 0x7BFF0)")
