# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# Phoscon RCP — host-side build (unit tests, tools).
#
# The firmware is a separate CMake project configured with the cross
# toolchain file:
#
#   cmake -B build/fw  -S src -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#   cmake --build build/fw
#
# Host tests (added as core modules land):
#
#   cmake -B build/test -S .
#   cmake --build build/test && ctest --test-dir build/test
cmake_minimum_required(VERSION 3.16)
project(phoscon-rcp-host C)

enable_testing()

# Test sources live in test/ (created with the first core module).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt)
  add_subdirectory(test)
endif()
