/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* ESP32-C6 app image under the board's ESP-IDF second-stage bootloader (docs/hardware_esp32.md §12.3).
 *
 * The bootloader asserts exactly two flash-mapped segments on this part, whose instruction and data
 * buses share one virtual window (SOC_MMU_DI_VADDR_SHARED), and requires the first to begin with the app
 * descriptor: so the read-only data, descriptor first, is one window and the code another, each its own
 * run of 64 KiB MMU pages. esptool's elf2image puts the segment holding .flash.appdesc first in the
 * file whatever its address, and pads the file so that each flash segment's offset equals its virtual
 * address modulo the page; 0x20 is where the first segment's data lands after the 24-byte image header
 * and its 8-byte segment header.
 *
 * RAM is one region, instruction and data alike, and ends where the bootloader's own loader segment
 * starts (esp32_chip.h ESP32_APP_RAM_END): the bootloader is still running from above it while it loads
 * this image's RAM segments. Code that must not wait on a cache miss - the trap entry, the vectors and
 * the interrupt paths - is placed in .iram.text. */
ENTRY(_start)

MEMORY
{
    drom (r)   : ORIGIN = 0x42000020, LENGTH = 0x100000 - 0x20
    irom (rx)  : ORIGIN = 0x42100020, LENGTH = 0x100000 - 0x20
    ram (rwx)  : ORIGIN = 0x40800000, LENGTH = 0x4086E610 - 0x40800000
}

SECTIONS
{
    .flash.appdesc : ALIGN(4)
    {
        KEEP(*(.flash.appdesc))
    } > drom

    .flash.rodata : ALIGN(4)
    {
        *(.rodata .rodata.* .srodata .srodata.*)
        . = ALIGN(4);
    } > drom

    .flash.text : ALIGN(4)
    {
        *(.text .text.*)
        . = ALIGN(4);
    } > irom

    /* Vectors first: RAM starts on a 256-byte boundary, which the table needs. */
    .iram.text : ALIGN(256)
    {
        KEEP(*(.iram.text.vectors))
        KEEP(*(.iram.text.entry))
        *(.iram.text.trap)
        *(.iram.text .iram.text.*)
        . = ALIGN(4);
    } > ram

    .data : ALIGN(4)
    {
        _sdata = .;
        *(.data .data.*)
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
        . = ALIGN(4);
        _edata = .;
    } > ram

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > ram

    /* Not cleared at boot: a panic's mark survives the reset that follows it (hal/esp32/esp32_panic.c). */
    .noinit (NOLOAD) : ALIGN(4)
    {
        *(.noinit .noinit.*)
        . = ALIGN(4);
    } > ram

    /* The stack fills the rest of the region, down from its end. */
    _stack_top = ORIGIN(ram) + LENGTH(ram);
    ASSERT(_stack_top - _ebss >= 0x2000, "esp32c6: less than 8 KiB of RAM left for the stack")
    ASSERT((_stack_top & 0xF) == 0, "esp32c6: the stack top must be 16-byte aligned (RISC-V ilp32)")

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.riscv.attributes)
    }
}
