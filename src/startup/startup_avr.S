/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* startup_avr.S — reset and vectors for the ATmega256RFR2 (cbrb1, docs/hardware_avr.md §11.5).
 *
 * The RaspBee I bootloader enters this image with a jump to address 0, after it has moved the
 * vectors back to the application section and cleared EIND, with interrupts masked and the stack
 * wherever its own code left it (docs/hardware_avr.md §11.2). So slot 0 is the image's entry as well
 * as its reset vector, and nothing the bootloader left is trusted: the zero register, SREG, the stack
 * pointer, EIND and RAMPZ are all set here.
 *
 * The run-time is assembled from avr-gcc's numbered .init sections, which the linker script
 * (targets/cbrb1/atmega256rfr2.ld) places in order behind the table:
 *
 *   .init0  this file: the core registers and the stack
 *   .init4  libgcc's __do_copy_data and __do_clear_bss, pulled in by every object that has .data or
 *           .bss; they read the load image with ELPM, so .data may sit anywhere in flash
 *   .init9  this file: HAL_PreMain, which never returns (hal/hal.h)
 *
 * Each table slot is a jmp. A vector no C file defines is a weak alias of __bad_interrupt, which hands
 * the fault to def_handler (hal/avr/avr_panic.c). test/unit/test_avr_regs.py checks the table: slot 0
 * jumps to __init, then the slots 1..76 in order, which is the 77 avr-libc's _VECTORS_SIZE gives. */
#include "avr_regs.h"

    .macro vec n
    jmp __vector_\n
    .weak __vector_\n
    .set __vector_\n, __bad_interrupt
    .endm

    .section .vectors, "ax", @progbits
    .global __vectors
__vectors:
    jmp __init
    vec 1
    vec 2
    vec 3
    vec 4
    vec 5
    vec 6
    vec 7
    vec 8
    vec 9
    vec 10
    vec 11
    vec 12
    vec 13
    vec 14
    vec 15
    vec 16
    vec 17
    vec 18
    vec 19
    vec 20
    vec 21
    vec 22
    vec 23
    vec 24
    vec 25
    vec 26
    vec 27
    vec 28
    vec 29
    vec 30
    vec 31
    vec 32
    vec 33
    vec 34
    vec 35
    vec 36
    vec 37
    vec 38
    vec 39
    vec 40
    vec 41
    vec 42
    vec 43
    vec 44
    vec 45
    vec 46
    vec 47
    vec 48
    vec 49
    vec 50
    vec 51
    vec 52
    vec 53
    vec 54
    vec 55
    vec 56
    vec 57
    vec 58
    vec 59
    vec 60
    vec 61
    vec 62
    vec 63
    vec 64
    vec 65
    vec 66
    vec 67
    vec 68
    vec 69
    vec 70
    vec 71
    vec 72
    vec 73
    vec 74
    vec 75
    vec 76

    .section .init0, "ax", @progbits
    .global __init
__init:
    clr r1
    out AVR_OWN_IO(AVR_SREG), r1
    ldi r28, lo8(AVR_RAMEND)
    ldi r29, hi8(AVR_RAMEND)
    out AVR_OWN_IO(AVR_SPH), r29
    out AVR_OWN_IO(AVR_SPL), r28
    out AVR_OWN_IO(AVR_EIND), r1
    out AVR_OWN_IO(AVR_RAMPZ), r1

    .section .init9, "ax", @progbits
    call HAL_PreMain
    jmp __bad_interrupt

    .text
    .global __bad_interrupt
__bad_interrupt:
    jmp def_handler
