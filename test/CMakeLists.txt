# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
# Host-side unit tests: plain C, host toolchain, no QEMU and no cross compiler.
#
# They cover the target-agnostic modules of src/ - the ones whose contract the
# host tools depend on (record framing, binary streams). A module enters here
# when a behaviour exists that no firmware run can reach on its own (the record
# ring's overflow marker, for instance: bring-up emits ~47 records - stages,
# stopwatch milestones and register witnesses - against the 63-record ring,
# and it has already dropped one at a smaller size, so the overflow behaviour is
# not hypothetical).
#
#   cmake -B build/test -S .
#   cmake --build build/test && ctest --test-dir build/test

add_executable(test_dbg_queue
  test_dbg_queue.c
  ${CMAKE_SOURCE_DIR}/src/hal/dbg.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c
)
target_include_directories(test_dbg_queue PRIVATE
  ${CMAKE_SOURCE_DIR}/src/hal
  ${CMAKE_SOURCE_DIR}/test/fakes
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream
)
target_compile_options(test_dbg_queue PRIVATE -Wall -Wextra -std=c99)
add_test(NAME dbg_queue COMMAND test_dbg_queue)

# ---- rcp/ core: the Spinel transport codec -------------------------------
# First suite of the RCP core phase (docs/testing.md §2). The module is
# pure C over U_BStream with no hardware knowledge, so the host can drive it
# directly - which is what docs/testing.md §2 is about: no fake is needed here,
# only the vectors and the fault injections (coding-rules.md §4.4).
add_executable(test_hdlc
  unit/test_hdlc.c
  tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/rcp/hdlc.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c
)
target_include_directories(test_hdlc PRIVATE
  ${CMAKE_SOURCE_DIR}/test
  ${CMAKE_SOURCE_DIR}/test/tu
  ${CMAKE_SOURCE_DIR}/src/rcp
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream
)
target_compile_options(test_hdlc PRIVATE -Wall -Wextra -std=c99)
add_test(NAME hdlc COMMAND test_hdlc)

# ---- rcp/ core: the Spinel framing layer ---------------------------------
add_executable(test_spinel
  unit/test_spinel.c
  tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/rcp/spinel.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c
)
target_include_directories(test_spinel PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu
  ${CMAKE_SOURCE_DIR}/src/rcp
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream
)
target_compile_options(test_spinel PRIVATE -Wall -Wextra -std=c99)
add_test(NAME spinel COMMAND test_spinel)

# ---- rcp/ core: the property table ---------------------------------------
add_executable(test_props
  unit/test_props.c
  tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/rcp/props.c
  ${CMAKE_SOURCE_DIR}/src/rcp/props_debug.c
  ${CMAKE_SOURCE_DIR}/src/rcp/srcmatch.c
  ${CMAKE_SOURCE_DIR}/src/rcp/spinel.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c
)
target_include_directories(test_props PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu
  ${CMAKE_SOURCE_DIR}/src/rcp
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream
)
target_compile_options(test_props PRIVATE -Wall -Wextra -std=c99)
target_compile_definitions(test_props PRIVATE RCP_THREAD=1 RCP_TX_TIMING=1 RCP_CSL_ACCURACY_PPM=40 RCP_CSL_UNCERTAINTY_10US=20)
target_include_directories(test_props PRIVATE ${CMAKE_SOURCE_DIR}/src/lib/aes)
target_sources(test_props PRIVATE ${CMAKE_SOURCE_DIR}/src/rcp/mac_sec.c ${CMAKE_SOURCE_DIR}/src/lib/aes/aes_modes.c ${CMAKE_SOURCE_DIR}/src/lib/aes/aes_table.c)
add_test(NAME props COMMAND test_props)

# The same table on a board without Thread support (cbrb1, rb2): its wire surface must not move.
add_executable(test_props_no_thread
  unit/test_props.c
  tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/rcp/props.c
  ${CMAKE_SOURCE_DIR}/src/rcp/props_debug.c
  ${CMAKE_SOURCE_DIR}/src/rcp/srcmatch.c
  ${CMAKE_SOURCE_DIR}/src/rcp/spinel.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c
)
target_include_directories(test_props_no_thread PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu
  ${CMAKE_SOURCE_DIR}/src/rcp
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream
)
target_compile_options(test_props_no_thread PRIVATE -Wall -Wextra -std=c99)
add_test(NAME props_no_thread COMMAND test_props_no_thread)

# And on a board with MAC security but no timed transmit yet (cb2).
add_executable(test_props_no_timing
  unit/test_props.c
  tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/rcp/props.c
  ${CMAKE_SOURCE_DIR}/src/rcp/props_debug.c
  ${CMAKE_SOURCE_DIR}/src/rcp/srcmatch.c
  ${CMAKE_SOURCE_DIR}/src/rcp/spinel.c
  ${CMAKE_SOURCE_DIR}/src/rcp/mac_sec.c
  ${CMAKE_SOURCE_DIR}/src/lib/aes/aes_modes.c
  ${CMAKE_SOURCE_DIR}/src/lib/aes/aes_table.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c
)
target_include_directories(test_props_no_timing PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu
  ${CMAKE_SOURCE_DIR}/src/rcp
  ${CMAKE_SOURCE_DIR}/src/lib/aes
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream
)
target_compile_options(test_props_no_timing PRIVATE -Wall -Wextra -std=c99)
target_compile_definitions(test_props_no_timing PRIVATE RCP_THREAD=1)
add_test(NAME props_no_timing COMMAND test_props_no_timing)

# ---- rcp/ core: the session (the whole dialog on the host) -----------------
add_executable(test_session
  unit/test_session.c
  tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/rcp/session.c
  ${CMAKE_SOURCE_DIR}/src/rcp/props_debug.c
  ${CMAKE_SOURCE_DIR}/src/rcp/props.c
  ${CMAKE_SOURCE_DIR}/src/rcp/srcmatch.c
  ${CMAKE_SOURCE_DIR}/src/rcp/spinel.c
  ${CMAKE_SOURCE_DIR}/src/rcp/hdlc.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c
  ${CMAKE_SOURCE_DIR}/src/rcp/props_stream.c
)
target_include_directories(test_session PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu
  ${CMAKE_SOURCE_DIR}/src/rcp
  # session.h includes props_stream.h, which includes the radio HAL contract.
  ${CMAKE_SOURCE_DIR}/src/hal
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream
)
target_compile_options(test_session PRIVATE -Wall -Wextra -std=c99)
target_compile_definitions(test_session PRIVATE RCP_THREAD=1 RCP_TX_TIMING=1 RCP_CSL_ACCURACY_PPM=40 RCP_CSL_UNCERTAINTY_10US=20)
target_include_directories(test_session PRIVATE ${CMAKE_SOURCE_DIR}/src/lib/aes)
target_sources(test_session PRIVATE ${CMAKE_SOURCE_DIR}/src/rcp/mac_sec.c ${CMAKE_SOURCE_DIR}/src/lib/aes/aes_modes.c ${CMAKE_SOURCE_DIR}/src/lib/aes/aes_table.c)
add_test(NAME session COMMAND test_session)

# ---- lib/aes: the vendored library's own known answers ----------------------
add_executable(test_aes
  unit/test_aes.c
  tu/test_harness.c
  ${CMAKE_SOURCE_DIR}/src/lib/aes/aes_modes.c
  ${CMAKE_SOURCE_DIR}/src/lib/aes/aes_table.c
)
target_include_directories(test_aes PRIVATE ${CMAKE_SOURCE_DIR}/test/tu ${CMAKE_SOURCE_DIR}/src/lib/aes)
target_compile_options(test_aes PRIVATE -Wall -Wextra -std=c99)
add_test(NAME aes COMMAND test_aes)

# ---- rcp/ core: MAC transmit security ---------------------------------------
add_executable(test_mac_sec
  unit/test_mac_sec.c
  ${CMAKE_SOURCE_DIR}/src/rcp/srcmatch.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c
  tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/rcp/mac_sec.c
  ${CMAKE_SOURCE_DIR}/src/lib/aes/aes_modes.c
  ${CMAKE_SOURCE_DIR}/src/lib/aes/aes_table.c
)
target_include_directories(test_mac_sec PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream
  ${CMAKE_SOURCE_DIR}/src/rcp
  ${CMAKE_SOURCE_DIR}/src/lib/aes
)
target_compile_options(test_mac_sec PRIVATE -Wall -Wextra -std=c99)
add_test(NAME mac_sec COMMAND test_mac_sec)

# ---- rcp/ core: the STREAM_RAW RX layout --------------------------------
# radio.h is the HAL contract, so this suite needs no fake: a Radio_Frame built by hand
# in the test is exactly what the module consumes (docs/testing.md §2).
add_executable(test_props_stream
  unit/test_props_stream.c
  tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/rcp/props_stream.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c
)
target_include_directories(test_props_stream PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu
  ${CMAKE_SOURCE_DIR}/src/rcp
  ${CMAKE_SOURCE_DIR}/src/hal
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream
)
target_compile_options(test_props_stream PRIVATE -Wall -Wextra -std=c99)
add_test(NAME props_stream COMMAND test_props_stream)

# ---- rcp/ core: the 802.15.4 header, filter and Ack decision --------------
# The filter and the Ack decision are what the receive interrupt calls, so they are pure
# functions over a PSDU and a caller-owned config (docs/ack-pending-frame.md).
add_executable(test_mac unit/test_mac.c tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/rcp/mac.c
  ${CMAKE_SOURCE_DIR}/src/rcp/srcmatch.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c
)
target_include_directories(test_mac PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu ${CMAKE_SOURCE_DIR}/src/rcp
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream)
target_compile_options(test_mac PRIVATE -Wall -Wextra -std=c99)
add_test(NAME mac COMMAND test_mac)

# ---- rcp/ core: the source-match table ----------------------------------
# The table answers the one question the Ack's Frame Pending bit needs, and the interrupt
# reads it without a lock, so its full-pool and byte-order behaviour is a host contract.
add_executable(test_srcmatch unit/test_srcmatch.c tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/rcp/srcmatch.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c
)
target_include_directories(test_srcmatch PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu ${CMAKE_SOURCE_DIR}/src/rcp
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream)
target_compile_options(test_srcmatch PRIVATE -Wall -Wextra -std=c99)
add_test(NAME srcmatch COMMAND test_srcmatch)

# ---- rcp/ core: the energy-scan dwell machine ---------------------------
# A pure core, so no fake and no HAL: the suite drives it with nowMs and the two calls the glue
# makes when an action has an answer (docs/testing.md §2).
add_executable(test_scan unit/test_scan.c tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/rcp/scan.c
)
target_include_directories(test_scan PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu ${CMAKE_SOURCE_DIR}/src/rcp)
target_compile_options(test_scan PRIVATE -Wall -Wextra -std=c99)
add_test(NAME scan COMMAND test_scan)

# ---- rcp/ core: debug-record packing for the RCP log property ------------
add_executable(test_props_debug
  unit/test_props_debug.c
  ${CMAKE_SOURCE_DIR}/src/rcp/props_debug.c
)
target_include_directories(test_props_debug PRIVATE
  ${CMAKE_SOURCE_DIR}/src/rcp
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream
)
target_compile_options(test_props_debug PRIVATE -Wall -Wextra -std=c99)
add_test(NAME props_debug COMMAND test_props_debug)

# ---- hal/: the SERCOM USART line-rate divisor --------------------------------
# The one UART number the board cannot show without a host on the wire: the arithmetic baud divisor of datasheet Table 23-2.
add_executable(test_uart_baud
  unit/test_uart_baud.c
  tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/hal/uart_baud.c
)
target_include_directories(test_uart_baud PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu
  ${CMAKE_SOURCE_DIR}/src/hal
)
target_compile_options(test_uart_baud PRIVATE -Wall -Wextra -std=c99)
add_test(NAME uart_baud COMMAND test_uart_baud)

# ---- app/: the host flasher's reset request ----------------------------------
add_executable(test_flasher_trap
  unit/test_flasher_trap.c
  ${CMAKE_SOURCE_DIR}/src/app/flasher_trap.c
)
target_include_directories(test_flasher_trap PRIVATE
  ${CMAKE_SOURCE_DIR}/src/app
  ${CMAKE_SOURCE_DIR}/src/hal
  ${CMAKE_SOURCE_DIR}/test/fakes
)
target_compile_options(test_flasher_trap PRIVATE -Wall -Wextra -std=c99)
add_test(NAME flasher_trap COMMAND test_flasher_trap)

# ---- hal/mg21: the ConBee III register transcription --------------------------------------
# src/hal/mg21/mg21_regs.h is this tree's own transcription of the EFR32MG21 register map. It is
# compared here with test/data/mg21_regs_ref.h, the record tools/mg21_regs_gold.py generates by
# reading the layout out of the vendor's own device headers with offsetof (Simplicity SDK
# 2024.12.1, Zlib; not part of this repository). Offsets, field encodings, interrupt numbers and
# geometry are each matched, because a wrong offset is a different register and a wrong shift is a
# different field, and neither is visible in arithmetic tests.
add_executable(test_mg21_regs unit/test_mg21_regs.c tu/tu.c)
target_include_directories(test_mg21_regs PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu
  ${CMAKE_SOURCE_DIR}/src/hal/mg21
  ${CMAKE_SOURCE_DIR}/test/data)
target_compile_options(test_mg21_regs PRIVATE -Wall -Wextra -std=c99)
add_test(NAME mg21_regs COMMAND test_mg21_regs)

# ---- hal/mg21: the radio's whole register record (maintainers only) ------------------------
# tools/mg21_radio_gold.py writes the whole radio record, the blocks the vendor withdrew from its
# SDKs, to the untracked internal/test/data/mg21_radio_full.h, and only the subset the mg21_regs
# suite checks to test/data/mg21_radio_ref.h. Where the whole record and its suite are present,
# the suite holds it to the shape the radio back-ends rely on (docs/hardware_mg21.md §10.4): the
# bases and their non-secure aliases, a SET/CLR/TGL window for every plain register, field
# encodings that are the contiguous runs their shift claims, the 14-state radio state machine, and
# the registers a bring-up needs named.
set(MG21_RADIO_FULL ${CMAKE_SOURCE_DIR}/internal/test)
if(EXISTS ${MG21_RADIO_FULL}/data/mg21_radio_full.h AND EXISTS ${MG21_RADIO_FULL}/unit/test_mg21_radio_map.c)
  add_executable(test_mg21_radio_map ${MG21_RADIO_FULL}/unit/test_mg21_radio_map.c tu/tu.c)
  target_include_directories(test_mg21_radio_map PRIVATE
    ${CMAKE_SOURCE_DIR}/test/tu
    ${MG21_RADIO_FULL}/data)
  target_compile_options(test_mg21_radio_map PRIVATE -Wall -Wextra -std=c99)
  add_test(NAME mg21_radio_map COMMAND test_mg21_radio_map)
endif()

# ---- hal/mg21: the Series 2 USART line-rate divisor ---------------------------------------
# The ConBee III's USART divides its input clock with a fractional divisor and samples each
# bit oversampled, which is a different equation from the SERCOM's (hal/uart_baud.c covers
# that one). The expectations are computed from the equation itself, the achieved rate is
# recomputed from the register value, and the refusals are the cases where no register value
# can express what was asked.
add_executable(test_usart_baud unit/test_usart_baud.c tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/hal/mg21/usart_baud.c)
target_include_directories(test_usart_baud PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu ${CMAKE_SOURCE_DIR}/src/hal/mg21)
target_compile_options(test_usart_baud PRIVATE -Wall -Wextra -std=c99)
add_test(NAME usart_baud COMMAND test_usart_baud)

# ---- hal/mg21: the ConBee III boot record --------------------------------------------------
# The rules of the record the vendor bootloader and the application share: the signature that
# makes a record count, the reason that asks to stay in the bootloader, and the fact that a
# cold boot classifies as "no record". Pure functions over a caller's buffer, so the contract
# docs/hardware_mg21.md §10.7 states is checkable without a board.
add_executable(test_mg21_reset_record unit/test_mg21_reset_record.c tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/hal/mg21/mg21_reset_record.c)
target_include_directories(test_mg21_reset_record PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu ${CMAKE_SOURCE_DIR}/src/hal/mg21)
target_compile_options(test_mg21_reset_record PRIVATE -Wall -Wextra -std=c99)
add_test(NAME mg21_reset_record COMMAND test_mg21_reset_record)

# The factory EUI-64 out of the user data page's manufacturing token: byte order and the values that
# mean "no address" (docs/hardware_mg21.md §10.2).
add_executable(test_mg21_ident unit/test_mg21_ident.c tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/hal/mg21/mg21_ident.c)
target_include_directories(test_mg21_ident PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu ${CMAKE_SOURCE_DIR}/src/hal/mg21)
target_compile_options(test_mg21_ident PRIVATE -Wall -Wextra -std=c99)
add_test(NAME mg21_ident COMMAND test_mg21_ident)

# ---- hal/avr: the ConBee I / RaspBee I factory EUI-64 out of the EEPROM's NV basic set ------------
# Magic, id, length, both CRC versions, the MAC byte order per version, the early 0xDE00 magic and the
# older big-endian prefix, against the application note's recovery example and vectors computed
# outside this tree's C code (docs/hardware_avr.md §11.3).
add_executable(test_avr_ident unit/test_avr_ident.c tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/hal/avr/avr_ident.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c)
target_include_directories(test_avr_ident PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu ${CMAKE_SOURCE_DIR}/src/hal/avr ${CMAKE_SOURCE_DIR}/src/lib/u_bstream)
target_compile_options(test_avr_ident PRIVATE -Wall -Wextra -std=c99)
add_test(NAME avr_ident COMMAND test_avr_ident)

# ---- hal/samr21: the deCONZ NVRAM basic set ----------------------------------------------------
# The host-set MAC in the last flash page, under the SAMR21 vendor firmware's rules: magic, id, length,
# the two CRCs (the application note's own example for id 2) and the old big-endian prefix.
add_executable(test_samr21_basic unit/test_samr21_basic.c tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/hal/samr21/samr21_basic.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c)
target_include_directories(test_samr21_basic PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu ${CMAKE_SOURCE_DIR}/src/hal/samr21 ${CMAKE_SOURCE_DIR}/src/lib/u_bstream)
target_compile_options(test_samr21_basic PRIVATE -Wall -Wextra -std=c99)
add_test(NAME samr21_basic COMMAND test_samr21_basic)

# ---- hal/avr: the USART divisor ---------------------------------------------------------------
# UBRR and the U2Xn choice against the datasheet's own examples (Tables 23-14 to 23-16), the receiver
# error the datasheet recommends for eight data bits, and the 12-bit field (docs/hardware_avr.md §11.2).
add_executable(test_avr_baud unit/test_avr_baud.c tu/tu.c ${CMAKE_SOURCE_DIR}/src/hal/avr/avr_baud.c)
target_include_directories(test_avr_baud PRIVATE ${CMAKE_SOURCE_DIR}/test/tu ${CMAKE_SOURCE_DIR}/src/hal/avr)
target_compile_options(test_avr_baud PRIVATE -Wall -Wextra -std=c99)
add_test(NAME avr_baud COMMAND test_avr_baud)

# ---- hal/avr: the microsecond clock's arithmetic ------------------------------------------------
add_executable(test_avr_clock_us unit/test_avr_clock_us.c tu/tu.c)
target_include_directories(test_avr_clock_us PRIVATE ${CMAKE_SOURCE_DIR}/test/tu ${CMAKE_SOURCE_DIR}/src/hal/avr)
target_compile_options(test_avr_clock_us PRIVATE -Wall -Wextra -std=c99)
add_test(NAME avr_clock_us COMMAND test_avr_clock_us)

# ---- hal/mg21/own: the radio's arithmetic and record formats ----------------------------------------
# The synthesiser word per channel against the vendor tune's own sixteen answers, the FCS the frame
# controller does not store against an independent implementation, and the receive record's layout
# against a record the bench's buffer held (docs/hardware_mg21.md §10.5).
add_executable(test_mg21_phy unit/test_mg21_phy.c tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/hal/mg21/own/mg21_phy.c)
target_include_directories(test_mg21_phy PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu ${CMAKE_SOURCE_DIR}/src/hal/mg21/own ${CMAKE_SOURCE_DIR}/src/hal
  ${CMAKE_SOURCE_DIR}/src/rcp ${CMAKE_SOURCE_DIR}/src/lib/u_bstream)
target_compile_options(test_mg21_phy PRIVATE -Wall -Wextra -std=c99)
add_test(NAME mg21_phy COMMAND test_mg21_phy)

# ---- hal/mg21/rail: the RAIL back-end's power curves ---------------------------------------------
# The dBm-to-level lookup over the xG21's published piecewise-linear curves (docs/hardware_mg21.md
# §10.19). Pure arithmetic, so it runs here although the RAIL back-end itself does not.
add_executable(test_mg21_rail_pa unit/test_mg21_rail_pa.c tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/hal/mg21/rail/mg21_rail_pa.c)
target_include_directories(test_mg21_rail_pa PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu ${CMAKE_SOURCE_DIR}/src/hal/mg21/rail)
target_compile_options(test_mg21_rail_pa PRIVATE -Wall -Wextra -std=c99)
add_test(NAME mg21_rail_pa COMMAND test_mg21_rail_pa)

# ---- tools/mg21_radio_gen.py: the radio back-end's generated data is current --------------------
# The sequencer image in src/ is generated from tools/mg21_seq.py; an edit to the program that was
# not regenerated is caught here. The register tables are checked too where the research corpus
# they are generated from is present (the maintainers' research corpus, not part of this repository).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME mg21_radio_gen
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/mg21_radio_gen.py --check)
endif()

# ---- targets/cb3: the ConBee III layout, checked as arithmetic ----------------------------
# The ConBee III has no image to link yet, but its layout is arithmetic that can be wrong, and a wrong
# region is a board in DFU: the app budget is the AppSection minus the AppInfo trailer the vendor
# bootloader writes at its end. This runs src/targets/cb3/flash_layout.cmake standalone - no cross
# toolchain, no board - which asserts the regions the bootloader's formula produces, that both cb3
# linker scripts end their FLASH region where the trailer begins, and that both reserve the 32-byte
# reset record at the bottom of RAM instead of letting .data land on it.
add_test(NAME cb3_layout
  COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/src/targets/cb3/flash_layout.cmake)

# ---- hal/mg21: the ConBee III millisecond, as arithmetic -----------------------------------------
# SysTick's reload is 24 bits and this board's core clock is 38.4 MHz, so a whole millisecond of it
# does not fit the register - the fact a port from the SAMR21 gets wrong by instinct. The plan module
# answers which clock the tick comes from and how a millisecond is split, and this suite checks every
# answer for the property that makes the board's clock honest: periods times cycles equals the cycles
# in a millisecond, exactly, and a clock that cannot provide one is refused rather than rounded.
add_executable(test_tick_plan unit/test_tick_plan.c tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/hal/mg21/tick_plan.c)
target_include_directories(test_tick_plan PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu ${CMAKE_SOURCE_DIR}/src/hal/mg21)
target_compile_options(test_tick_plan PRIVATE -Wall -Wextra -std=c99)
add_test(NAME tick_plan COMMAND test_tick_plan)

# ---- startup/: the ConBee III vector table, checked against the part --------------------------------
# An entry in the wrong slot hands one peripheral's interrupt to another driver's handler and no
# register read says so. The record is generated from the part's own IRQn_Type enum, and this suite
# compares the startup table with it slot by slot - the automation of docs/testing.md §5 audit G.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME mg21_vectors
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/test/unit/test_mg21_vectors.py)
endif()

# ---- tools/: the pre-flash guard ---------------------------------------------------------------
# The guard is what stands between a wrong container and a board whose bootloader was overwritten, and
# it is host code, not firmware - so every rule it enforces gets a container that satisfies it and one
# that does not. The containers are built by the test: the guard checks structure and checksums and
# never looks inside the payload, so nothing from a board or a vendor artifact is needed.
if(Python3_Interpreter_FOUND)
  add_test(NAME gcf_guard
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/test/unit/test_gcf_guard.py)
  # Every tracked tool that flashes a board runs that guard for the board it flashes.
  add_test(NAME flash_paths
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/test/unit/test_flash_paths.py)
endif()

# ---- tools/rcp_throughput.py: the estimate's byte counts and the closed loop -------------------
# The estimate is only as good as the bytes it counts, so its frames are checked against the reference
# captures of docs/protocol.md; the loop runs against a fake RCP pair, each outcome in its own count.
if(Python3_Interpreter_FOUND)
  add_test(NAME rcp_throughput
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/test/unit/test_rcp_throughput.py)
endif()

# ---- the portable code with a 16-bit int (cbrb1) ------------------------------------------------
# The ATmega256RFR2 runs the same rcp/, app/, com/uart.c and hal/dbg.c with avr-gcc's 16-bit int,
# which the host suites never see. The host clang type-checks every portable source for the AVR and
# for the Cortex-M0+; a warning only the AVR pass gives is an int-width bug. Skipped (77) when the
# host clang has no AVR target.
if(Python3_Interpreter_FOUND)
  add_test(NAME avr_int
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/test/unit/test_avr_int.py)
  set_tests_properties(avr_int PROPERTIES SKIP_RETURN_CODE 77)
endif()

# ---- hal/avr: the ATmega256RFR2 register transcription and the vector table ---------------------
# src/hal/avr/avr_regs.h is this tree's own register map; every AVR_<name> in it is compared with
# avr-libc's <name> by the assembler, and startup_avr.S's table is checked slot by slot against the
# part's vector count. Skipped (77) without avr-gcc.
if(Python3_Interpreter_FOUND)
  add_test(NAME avr_regs
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/test/unit/test_avr_regs.py)
  set_tests_properties(avr_regs PROPERTIES SKIP_RETURN_CODE 77)
endif()

# ---- tools/dbg.py: the witness field tables decode their own fields -----------------------------
# The debug channel is the board's only witness; a mask that decodes a constant 0 or a label that
# names an impossible value is a silent lie. Every field is checked structurally and a set of pinned
# register halves decode to their expected text.
if(Python3_Interpreter_FOUND)
  add_test(NAME dbg_decoder
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/test/unit/test_dbg_decoder.py)
endif()

# ---- the AppSection size guard, on both sides of its bound -------------------------------------
# The guard has only ever printed its pass line: the linker's FLASH region ends at the same address,
# so an image that big fails the link before the guard would see it. This feeds it a bin at the
# bound (passes) and one byte over (FATAL_ERROR), so the failure path is proven, not assumed.
if(Python3_Interpreter_FOUND)
  add_test(NAME image_guard
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/test/unit/test_image_guard.py)
endif()

# ---- app/ core: the bench register monitor's command format --------------
# The probe image's register monitor (src/app/mon_proto.c, docs/hardware_mg21.md §10.5) decides
# what a command touches before the board's glue touches it, so its parser and its address policy
# are the parts with a contract: exact lengths, bounded counts, the read-only hole inside a writable
# window, and no range that wraps the address space.
add_executable(test_mon_proto
  unit/test_mon_proto.c
  tu/tu.c
  ${CMAKE_SOURCE_DIR}/src/app/mon_proto.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c
)
target_include_directories(test_mon_proto PRIVATE
  ${CMAKE_SOURCE_DIR}/test/tu
  ${CMAKE_SOURCE_DIR}/src/app
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream
)
target_compile_options(test_mon_proto PRIVATE -Wall -Wextra -std=c99)
add_test(NAME mon_proto COMMAND test_mon_proto)

# ---- app/ glue: rcp_app.c under hostile input --------------------------------------------
# The glue between the link, the session and the radio, on the fakes of test/fakes/: no input the
# host link carries may cost the board its watchdog (test_rcp_app.c).
add_executable(test_rcp_app
  unit/test_rcp_app.c
  tu/tu.c
  fakes/fake_board.c
  fakes/fake_com.c
  fakes/fake_radio.c
  ${CMAKE_SOURCE_DIR}/src/app/rcp_app.c
  ${CMAKE_SOURCE_DIR}/src/app/main_rcp.c
  ${CMAKE_SOURCE_DIR}/src/hal/dbg.c
  ${CMAKE_SOURCE_DIR}/src/rcp/hdlc.c
  ${CMAKE_SOURCE_DIR}/src/rcp/mac.c
  ${CMAKE_SOURCE_DIR}/src/rcp/mac_sec.c
  ${CMAKE_SOURCE_DIR}/src/rcp/props.c
  ${CMAKE_SOURCE_DIR}/src/rcp/props_debug.c
  ${CMAKE_SOURCE_DIR}/src/rcp/props_stream.c
  ${CMAKE_SOURCE_DIR}/src/rcp/scan.c
  ${CMAKE_SOURCE_DIR}/src/rcp/session.c
  ${CMAKE_SOURCE_DIR}/src/rcp/spinel.c
  ${CMAKE_SOURCE_DIR}/src/rcp/srcmatch.c
  ${CMAKE_SOURCE_DIR}/src/lib/aes/aes_modes.c
  ${CMAKE_SOURCE_DIR}/src/lib/aes/aes_table.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream/u_bstream.c
  ${CMAKE_SOURCE_DIR}/src/lib/u_sstream/u_sstream.c
)
target_include_directories(test_rcp_app PRIVATE
  ${CMAKE_SOURCE_DIR}/test/fakes
  ${CMAKE_SOURCE_DIR}/test/tu
  ${CMAKE_SOURCE_DIR}/src/app
  ${CMAKE_SOURCE_DIR}/src/board
  ${CMAKE_SOURCE_DIR}/src/hal
  ${CMAKE_SOURCE_DIR}/src/rcp
  ${CMAKE_SOURCE_DIR}/src/lib/aes
  ${CMAKE_SOURCE_DIR}/src/lib/u_bstream
  ${CMAKE_SOURCE_DIR}/src/lib/u_sstream
)
target_compile_options(test_rcp_app PRIVATE -Wall -Wextra -std=c99)
target_compile_definitions(test_rcp_app PRIVATE RCP_THREAD=1 RCP_TX_TIMING=1 RCP_CSL_ACCURACY_PPM=40 RCP_CSL_UNCERTAINTY_10US=20)
add_test(NAME rcp_app COMMAND test_rcp_app)
