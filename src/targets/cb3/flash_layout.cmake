# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# The ConBee III flash layout, as arithmetic rather than as a table.
#
# Included by targets/cb3/board.cmake, and runnable on its own (`cmake -P flash_layout.cmake`) so a
# host test can check it without a cross toolchain or a board (docs/testing.md §2).
#
# The formula is the bootloader's - the deSiLabs bootloader's de_BTL_flash_getFlashInfo():
#   appNvm      = 4 pages
#   imageSector = ((flashSize - appNvm) / 2 / pageSize) * pageSize   (ImageSector must hold BTL + app)
#   appSector   = imageSector - btlSector
#   appOrigin   = btlSector
#   imageOrigin = btlSector + appSector
#
# The inputs are the part and the bootloader's constants: EFR32MG21A010F1024IM32 is 1 MiB flash with
# 8 KiB erase pages (SDK part header); DE_BTL_FLASH_BTL_SECTOR_SIZE_BYTES is 16 KiB,
# DE_BTL_FLASH_APPNVM_SECTOR_SIZE_BYTES is 4 pages, and AppInfo is four u32 at the end of the
# AppSection (de_BTL_flash_setAppInfo).
set(CB3_FLASH_SIZE      0x100000)
set(CB3_FLASH_PAGE      0x2000)
set(CB3_BTL_SECTOR      0x4000)
set(CB3_APP_NVM_PAGES   4)
set(CB3_APPINFO_SIZE    16)

math(EXPR CB3_APP_NVM      "${CB3_APP_NVM_PAGES} * ${CB3_FLASH_PAGE}")
math(EXPR CB3_IMAGE_SECTOR "(${CB3_FLASH_SIZE} - ${CB3_APP_NVM}) / 2 / ${CB3_FLASH_PAGE}")
math(EXPR CB3_IMAGE_SECTOR "${CB3_IMAGE_SECTOR} * ${CB3_FLASH_PAGE}")
math(EXPR CB3_APP_SECTOR   "${CB3_IMAGE_SECTOR} - ${CB3_BTL_SECTOR}")
math(EXPR CB3_APP_ORIGIN   "${CB3_BTL_SECTOR}")
math(EXPR CB3_IMAGE_ORIGIN "${CB3_BTL_SECTOR} + ${CB3_APP_SECTOR}")
math(EXPR CB3_NVM_ORIGIN   "${CB3_IMAGE_ORIGIN} + ${CB3_IMAGE_SECTOR}")
math(EXPR CB3_IMAGE_BOUND  "${CB3_APP_ORIGIN} + ${CB3_APP_SECTOR} - ${CB3_APPINFO_SIZE}")
math(EXPR CB3_APP_BUDGET   "${CB3_IMAGE_BOUND} - ${CB3_APP_ORIGIN}")

# These are the regions docs/hardware_mg21.md §10.7 records and tools/gcf_check.py and GCFTool's cb3 profile
# guard against. The comparison is the point: if the formula stops producing them, a documented fact
# about this board is wrong, and the build must stop rather than link an image into the wrong place.
foreach(_pair "CB3_APP_SECTOR:0x78000" "CB3_IMAGE_SECTOR:0x7C000" "CB3_IMAGE_ORIGIN:0x7C000"
             "CB3_NVM_ORIGIN:0xF8000" "CB3_IMAGE_BOUND:0x7BFF0" "CB3_APP_BUDGET:0x77FF0")
  string(REPLACE ":" ";" _pair ${_pair})
  list(GET _pair 0 _var)
  list(GET _pair 1 _want)
  if(NOT (${_var} EQUAL ${_want}))
    message(FATAL_ERROR "cb3: ${_var} computes to ${${_var}}, expected ${_want}. The layout in "
                        "docs/hardware_mg21.md §10.7 no longer matches the bootloader's formula for this part.")
  endif()
endforeach()

# Why the bound leaves 16 bytes out: the bootloader writes its AppInfo trailer at the end of the
# AppSection after the app. An image that reaches the trailer is flashed, then fails
# de_BTL_flash_verifyApp against the trailer it overwrote - the board sits in DFU holding an image the
# host believed in. 491 504 B is the app budget, and it is the same limit the container guard applies.
# RAM carries its own hazard, so it is checked the same way. The bootloader's reset record sits at
# 0x20000000 - the first 32 bytes of RAM (docs/hardware_mg21.md §10.7) - and an image that lets .data or .bss start
# there destroys the reason it booted before it can read it. The script therefore declares a RECORD
# region of 32 bytes at the bottom of RAM and start usable RAM above it, and places a NOLOAD
# .reset_record in the region.
foreach(_ld efr32mg21.ld)
  file(READ "${CMAKE_CURRENT_LIST_DIR}/${_ld}" _text)
  string(REGEX MATCH "RECORD[ ]*\\(xrw\\)[ ]*: ORIGIN = 0x([0-9A-Fa-f]+), LENGTH = 0x([0-9A-Fa-f]+)" _m "${_text}")
  if(NOT _m)
    message(FATAL_ERROR "cb3: ${_ld} no longer reserves the reset record as its own region.")
  endif()
  if(NOT (0x${CMAKE_MATCH_1} EQUAL 0x20000000))
    message(FATAL_ERROR "cb3: ${_ld} puts the reset record at ${CMAKE_MATCH_1}, but the bootloader reads it at 0x20000000.")
  endif()
  if(NOT (0x${CMAKE_MATCH_2} EQUAL 32))
    message(FATAL_ERROR "cb3: ${_ld} reserves ${CMAKE_MATCH_2} bytes for the reset record; it is 16 words, 32 bytes.")
  endif()
  if(NOT _text MATCHES "\\.reset_record \\(NOLOAD\\)")
    message(FATAL_ERROR "cb3: ${_ld} has no NOLOAD .reset_record section, so the region is named but not used.")
  endif()
  string(REGEX MATCH "RAM[ ]*\\(xrw\\)[ ]*: ORIGIN = 0x([0-9A-Fa-f]+), LENGTH = (0x[0-9A-Fa-f]+) - (0x[0-9A-Fa-f]+)" _m2 "${_text}")
  if(NOT _m2)
    message(FATAL_ERROR "cb3: ${_ld} no longer carves the usable RAM out of the 96 KiB by subtraction.")
  endif()
  math(EXPR _ram_end "0x${CMAKE_MATCH_1} + (${CMAKE_MATCH_2} - ${CMAKE_MATCH_3})")
  if(NOT (0x${CMAKE_MATCH_1} EQUAL 0x20000020))
    message(FATAL_ERROR "cb3: ${_ld} starts usable RAM at ${CMAKE_MATCH_1}, which is not just above the reset record.")
  endif()
  if(NOT (_ram_end EQUAL 0x20018000))
    message(FATAL_ERROR "cb3: ${_ld} ends RAM at ${_ram_end}, but this part has 96 KiB ending at 0x20018000.")
  endif()
  math(EXPR _usable_ram "${CMAKE_MATCH_2} - ${CMAKE_MATCH_3}")
endforeach()

message(STATUS "cb3 layout: app 0x4000 for ${CB3_APP_SECTOR} B, staging 0x7C000 for ${CB3_IMAGE_SECTOR} B, "
               "NVM3 0xF8000, image bound 0x7BFF0 (app budget ${CB3_APP_BUDGET} B, AppInfo trailer "
               "${CB3_APPINFO_SIZE} B), RAM 0x20000000 with the reset record reserved at its start and "
               "${_usable_ram} B usable - the hex literals are the asserted values, the byte counts are computed")

# The linker scripts state the bound literally, so check the literals against the arithmetic: an image
# region must end exactly where the AppInfo trailer begins, in the GCF layout. A hand-edited .ld that moves the end fails here, not on the board.
set(CB3_LD_END_OK 1)
foreach(_ld efr32mg21.ld)
  file(READ "${CMAKE_CURRENT_LIST_DIR}/${_ld}" _text)
  string(REGEX MATCH "FLASH[ ]*\\(rx\\)[ ]*: ORIGIN = 0x([0-9A-Fa-f]+), LENGTH = 0x([0-9A-Fa-f]+) - 0x([0-9A-Fa-f]+)" _m "${_text}")
  if(NOT _m)
    message(FATAL_ERROR "cb3: ${_ld} no longer has the FLASH region this check expects.")
  endif()
  math(EXPR _end "0x${CMAKE_MATCH_1} + (0x${CMAKE_MATCH_2} - 0x${CMAKE_MATCH_3})")
  if(NOT (_end EQUAL CB3_IMAGE_BOUND))
    message(FATAL_ERROR "cb3: ${_ld} ends its FLASH region at ${_end} B, but the AppInfo trailer begins at "
                        "${CB3_IMAGE_BOUND} B (0x7BFF0): the image would be written over it.")
  endif()
endforeach()
