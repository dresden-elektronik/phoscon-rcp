/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* atmega256rfr2.ld — the cbrb1 image: the application section of the ATmega256RFR2 below the
 * RaspBee I bootloader (docs/hardware_avr.md §11.1, §11.10, §11.12).
 *
 * Flash: the application owns 0x00000..0x3DFFF; the bootloader's 8 KiB boot section at 0x3E000 is
 * never part of the image, and the region ends there so a link that reaches it fails here, before the
 * image guard and tools/gcf_check.py --board cbrb1 refuse the same image again.
 *
 * SRAM: 0x200..0x81FF. avr-gcc addresses data space at 0x800000 in its linker's view, so the RAM
 * region is written there; a pointer the C code forms has the 16-bit address without the offset.
 *
 * .rodata is placed with .data: avr-gcc reads a const object with ordinary loads, so it has to be in
 * RAM, and libgcc's __do_copy_data copies it there with the rest. The image's few hundred bytes of it
 * are a small part of 32 KiB. No EEPROM section is produced: the image never writes the EEPROM, and
 * the bootloader writes only flash.
 *
 * .noinit is not cleared or copied: the panic mark (hal/avr/avr_panic.c) is read back from it after a
 * watchdog reset. It is placed last, above .bss, which keeps it clear of the bootloader's own globals
 * at the bottom of SRAM. Whether the bootloader's stack, at the top, stays clear of it is not
 * measured. */
OUTPUT_FORMAT("elf32-avr", "elf32-avr", "elf32-avr")
OUTPUT_ARCH(avr:6)
ENTRY(__vectors)

MEMORY
{
    text   (rx)   : ORIGIN = 0x000000, LENGTH = 0x3E000
    data   (rw!x) : ORIGIN = 0x800200, LENGTH = 0x8000
}

__stack = 0x81FF;

SECTIONS
{
    .text :
    {
        KEEP(*(.vectors))
        *(.progmem.gcc*)
        *(.progmem*)
        . = ALIGN(2);
        __trampolines_start = .;
        *(.trampolines*)
        __trampolines_end = .;
        *(.jumptables*)
        *(.lowtext*)
        KEEP(*(.init0))
        KEEP(*(.init1))
        KEEP(*(.init2))
        KEEP(*(.init3))
        KEEP(*(.init4))
        KEEP(*(.init5))
        KEEP(*(.init6))
        KEEP(*(.init7))
        KEEP(*(.init8))
        KEEP(*(.init9))
        *(.text)
        . = ALIGN(2);
        *(.text.*)
        . = ALIGN(2);
        *(.fini*)
        _etext = .;
    } > text

    .data :
    {
        PROVIDE(__data_start = .);
        *(.data)
        *(.data*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(2);
        PROVIDE(__data_end = .);
    } > data AT > text

    __data_load_start = LOADADDR(.data);
    __data_load_end = __data_load_start + SIZEOF(.data);

    .bss (NOLOAD) :
    {
        PROVIDE(__bss_start = .);
        *(.bss)
        *(.bss*)
        *(COMMON)
        PROVIDE(__bss_end = .);
    } > data

    .noinit (NOLOAD) :
    {
        PROVIDE(__noinit_start = .);
        *(.noinit*)
        PROVIDE(__noinit_end = .);
        __heap_start = .;
    } > data

    /DISCARD/ : { *(.eeprom*) *(.fuse*) *(.lock*) *(.signature*) }

    .comment 0 : { *(.comment) }
    .note.gnu.avr.deviceinfo 0 : { KEEP(*(.note.gnu.avr.deviceinfo)) }
}
