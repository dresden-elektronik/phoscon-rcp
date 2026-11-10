# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# An ESP32 board's radio PHY: fetch Espressif's PHY libraries, name what the build needs of them.
#
# Included by an ESP32 board's board.cmake once it drives the radio (docs/build-and-flash.md §13). The
# archives, the ROM address lists and the default init table are Espressif's, fetched once into a cache
# outside the tree by tools/fetch_esp_phy.py, every file pinned by SHA-256; nothing of them enters the
# repository. The board sets DS_ESP32_CHIP (the chip's directory name under hal/esp32/, and the name the
# fetched files carry) before it includes this file. What this file hands back, by name:
#   DS_ESP32_PHY_SOURCES   this tree's own glue (hal/esp32/phy/): the bring-up sequence and the shims
#   DS_LINK_OPTIONS        the ROM's PHY address list and the two ROM routines the glue calls
#   DS_LINK_LIBRARIES      the two archives
# and the init table's include path, on the glue's translation unit only.

set(DS_ESP_PHY_CACHE "" CACHE PATH
    "where tools/fetch_esp_phy.py keeps the PHY libraries (default: ~/.cache/phoscon-rcp/esp-phy)")

find_program(DS_ESP_PHY_PYTHON NAMES python3 python)
if(NOT DS_ESP_PHY_PYTHON)
  message(FATAL_ERROR "an ESP32 radio needs Python 3 to run tools/fetch_esp_phy.py")
endif()

set(_phy_fetch ${DS_ESP_PHY_PYTHON} -I ${DS_SRC_DIR}/../tools/fetch_esp_phy.py)
if(DS_ESP_PHY_CACHE)
  list(APPEND _phy_fetch --cache ${DS_ESP_PHY_CACHE})
endif()

execute_process(COMMAND ${_phy_fetch}
                RESULT_VARIABLE _phy_rc OUTPUT_VARIABLE _phy_out ERROR_VARIABLE _phy_err)
if(NOT _phy_rc EQUAL 0)
  message(FATAL_ERROR "the ESP32 PHY libraries could not be fetched:\n${_phy_err}")
endif()

string(REGEX MATCH "ESP_PHY_ROOT=([^\r\n]+)" _phy_match "${_phy_out}")
set(DS_ESP_PHY_ROOT "${CMAKE_MATCH_1}")
if(NOT DS_ESP_PHY_ROOT)
  message(FATAL_ERROR "tools/fetch_esp_phy.py did not report a tree:\n${_phy_out}")
endif()
message(STATUS "ESP32 PHY libraries: ${DS_ESP_PHY_ROOT}")

set(DS_ESP32_PHY_SOURCES
  "${DS_SRC_DIR}/hal/esp32/phy/esp32_phy.c"
  "${DS_SRC_DIR}/hal/esp32/phy/phy_shims.c"
)

list(APPEND DS_LINK_OPTIONS
  "-Wl,-T,${DS_ESP_PHY_ROOT}/esp-idf/${DS_ESP32_CHIP}.rom.phy.ld"
  "-Wl,-T,${DS_ESP_PHY_ROOT}/generated/${DS_ESP32_CHIP}.rom.extra.ld")
list(APPEND DS_LINK_LIBRARIES
  "${DS_ESP_PHY_ROOT}/esp-phy-lib/${DS_ESP32_CHIP}/libphy.a"
  "${DS_ESP_PHY_ROOT}/esp-phy-lib/${DS_ESP32_CHIP}/libbtbb.a")

set_source_files_properties("${DS_SRC_DIR}/hal/esp32/phy/esp32_phy.c" PROPERTIES
  COMPILE_OPTIONS "-I${DS_ESP_PHY_ROOT}/generated;-DESP32_PHY_INIT_DATA_INC=\"${DS_ESP32_CHIP}_phy_init_data.inc\"")
