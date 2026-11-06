# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# Cross toolchain: 8-bit AVR, the ATmega256RFR2 of the ConBee I and RaspBee I (cbrb1), freestanding.
#
# Separate from the ARM files because the core differs (docs/hardware_avr.md §11.8): 16-bit int and pointers,
# and a Harvard machine whose flash is read with its own instructions. Startup is supplied by the
# board's DS_STARTUP (startup/startup_avr.S), not by avr-libc's crt; the image links neither avr-libc
# nor its device library, only libgcc, whose .init4 routines copy .data and clear .bss for the
# startup (docs/hardware_avr.md §11.5).
#
# The compiler is avr-gcc from the osx-cross/avr Homebrew tap. Its versioned formula is keg-only, so
# it is looked for in the keg as well as on PATH; avr-binutils links into the Homebrew prefix.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR avr)

set(DS_AVR_HINTS
  /opt/homebrew/opt/avr-gcc@14/bin
  /usr/local/opt/avr-gcc@14/bin
  /opt/homebrew/bin
  /usr/local/bin)

find_program(DS_AVR_GCC NAMES avr-gcc HINTS ${DS_AVR_HINTS})
find_program(DS_AVR_OBJCOPY NAMES avr-objcopy HINTS ${DS_AVR_HINTS})
find_program(DS_AVR_SIZE NAMES avr-size HINTS ${DS_AVR_HINTS})

if(NOT DS_AVR_GCC OR NOT DS_AVR_OBJCOPY)
  message(FATAL_ERROR "avr-gcc or avr-objcopy not found: brew install osx-cross/avr/avr-gcc@14 "
                      "osx-cross/avr/avr-binutils (docs/build-and-flash.md §11)")
endif()

set(CMAKE_C_COMPILER   ${DS_AVR_GCC})
set(CMAKE_ASM_COMPILER ${DS_AVR_GCC})
set(CMAKE_OBJCOPY      ${DS_AVR_OBJCOPY})
set(CMAKE_SIZE         ${DS_AVR_SIZE})

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MCU_FLAGS "-mmcu=atmega256rfr2")
set(CMAKE_C_FLAGS_INIT    "${MCU_FLAGS} -Os -std=c99 -ffreestanding -ffunction-sections -fdata-sections -fno-common")
set(CMAKE_ASM_FLAGS_INIT  "${MCU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${MCU_FLAGS} -nostartfiles -nostdlib -Wl,--gc-sections")
