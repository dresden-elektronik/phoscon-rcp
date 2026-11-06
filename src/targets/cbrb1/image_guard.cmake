# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# The cbrb1 image guard: pad the flat image to a size the RaspBee I bootloader writes whole, then check
# it ends below the boot section (docs/hardware_avr.md §11.2, docs/build-and-flash.md §6).
#
# The bootloader takes the container's size field as the number of bytes to write: an odd size loses its
# last byte, and a size that is a multiple of the 256-byte page makes it ask for a page past the end,
# which GCFFlasher refuses. So the image is padded with erased-flash 0xFF bytes - one for an odd size,
# then two more if the size has landed on a page multiple - which writes nothing the image did not have.
# tools/gcf_check.py --board cbrb1 refuses a container that breaks either rule, so a .bin that bypassed
# this step cannot be flashed either.
#
# MAX_BYTES = 0x3E000 - 0x0 = 253952, computed by the shared build from DS_IMAGE_BOUND and the origin.
string(ASCII 255 _ff)   # one 0xFF byte: CMake strings are byte strings
file(SIZE ${BIN} _size)
math(EXPR _odd "${_size} % 2")
if(_odd)
  file(APPEND ${BIN} "${_ff}")
  math(EXPR _size "${_size} + 1")
endif()
math(EXPR _page "${_size} % 256")
if(_page EQUAL 0)
  file(APPEND ${BIN} "${_ff}${_ff}")
  math(EXPR _size "${_size} + 2")
endif()
file(SIZE ${BIN} _size)
math(EXPR _odd "${_size} % 2")
math(EXPR _page "${_size} % 256")
if(_odd OR _page EQUAL 0)
  message(FATAL_ERROR "${BIN} is ${_size} bytes after padding: still odd or a multiple of 256")
endif()
if(NOT _size LESS ${MAX_BYTES})
  message(FATAL_ERROR
    "${BIN} is ${_size} bytes; the image must be < ${MAX_BYTES} bytes (0x0..0x3DFFF). Shrink the app: "
    "the RaspBee I bootloader occupies 0x3E000..0x3FFFF and an image that reaches it is refused by "
    "tools/gcf_check.py.")
endif()
message(STATUS "image guard: ${BIN} = ${_size} bytes (< ${MAX_BYTES}, even, not a page multiple, ends before 0x3E000)")
