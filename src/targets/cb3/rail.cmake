# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# The ConBee III's RAIL radio back-end: fetch the vendor library, name what the build needs of it.
#
# Included by board.cmake for an rcp image on RAIL (docs/build-and-flash.md §10). The library and its
# headers are Silicon Labs', fetched once into a cache outside the tree by tools/fetch_rail.py, every
# file pinned by SHA-256; nothing of them enters the repository. What this file hands back to the shared
# build, by name:
#   DS_RAIL_SOURCES          this tree's own adapter (hal/mg21/rail/), in place of the from-scratch back-end
#   DS_COMPILE_OPTIONS       the hard-float ABI the archive was built for, for every translation unit
#   DS_LINK_OPTIONS          the same, so the link picks the matching libgcc
#   DS_LINK_LIBRARIES        the archive
# and the include paths of the adapter's translation unit, as a source property.

set(DS_RAIL_CACHE "" CACHE PATH
    "where tools/fetch_rail.py keeps the RAIL library (default: ~/.cache/phoscon-rcp/rail)")

find_program(DS_RAIL_PYTHON NAMES python3 python)
if(NOT DS_RAIL_PYTHON)
  message(FATAL_ERROR "DS_MG21_RADIO=rail needs Python 3 to run tools/fetch_rail.py")
endif()

set(_rail_fetch ${DS_RAIL_PYTHON} -I ${DS_SRC_DIR}/../tools/fetch_rail.py)
if(DS_RAIL_CACHE)
  list(APPEND _rail_fetch --cache ${DS_RAIL_CACHE})
endif()

execute_process(COMMAND ${_rail_fetch}
                RESULT_VARIABLE _rail_rc OUTPUT_VARIABLE _rail_out ERROR_VARIABLE _rail_err)
if(NOT _rail_rc EQUAL 0)
  message(FATAL_ERROR "DS_MG21_RADIO=rail: the RAIL library could not be fetched:\n${_rail_err}")
endif()

string(REGEX MATCH "RAIL_ROOT=([^\r\n]+)" _rail_match "${_rail_out}")
set(DS_RAIL_ROOT "${CMAKE_MATCH_1}")
if(NOT DS_RAIL_ROOT)
  message(FATAL_ERROR "DS_MG21_RADIO=rail: tools/fetch_rail.py did not report a tree:\n${_rail_out}")
endif()
message(STATUS "RAIL library: ${DS_RAIL_ROOT}")

set(_rail_lib "${DS_RAIL_ROOT}/platform/radio/rail_lib")
set(DS_RAIL_ARCHIVE "${_rail_lib}/autogen/librail_release/librail_efr32xg21_gcc_release.a")

set(DS_RAIL_SOURCES
  "${DS_SRC_DIR}/hal/mg21/rail/mg21_rail.c"
  "${DS_SRC_DIR}/hal/mg21/rail/mg21_rail_pa.c"
  "${DS_SRC_DIR}/hal/mg21/rail/rail_shims.c"
  "${DS_SRC_DIR}/hal/mg21/rail/mg21_aes.c"
)

# The archive is built for the hard-float ABI of the part's single-precision FPU, and the linker refuses
# to mix it with soft-float objects: the whole image follows. The FPU itself is enabled by the adapter
# (hal/mg21/rail/mg21_rail.c Radio_Init) before the first call into RAIL.
set(DS_COMPILE_OPTIONS -mfloat-abi=hard -mfpu=fpv5-sp-d16)
set(DS_LINK_OPTIONS -mfloat-abi=hard -mfpu=fpv5-sp-d16)
set(DS_LINK_LIBRARIES "${DS_RAIL_ARCHIVE}")

# RAIL's headers reach only the adapter's translation unit. The compat directory stands in for the two
# things they expect of a hosted target (an SDK device header and <string.h>) and comes first; the
# vendor tree is a system directory so its headers are not held to this tree's warning levels.
set_source_files_properties("${DS_SRC_DIR}/hal/mg21/rail/mg21_rail.c" PROPERTIES
  COMPILE_OPTIONS
    "-I${DS_SRC_DIR}/hal/mg21/rail/compat;-isystem;${_rail_lib}/common;-isystem;${_rail_lib}/chip/efr32/efr32xg2x;-isystem;${_rail_lib}/protocol/ieee802154;-isystem;${DS_RAIL_ROOT}/platform/common/inc")
