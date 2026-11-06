/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* RaspBee II (ATSAMR21E18A) application image.
 *
 * The layout is the ConBee II's: the vendor ships one binary for both products and its
 * container for this board targets 0x5000 as well (docs/boards.md §2). What is inherited
 * rather than measured here is the 20 KB below - this board's bootloader has never been
 * read (docs/hardware_samr21.md §9).
 *
 * Flash map (docs/build-and-flash.md §6):
 *   0x00000-0x05000  bootloader (vendor, never written by us)
 *   0x05000-0x05100  app vector table (our image starts here)
 *   0x05100-0x09100  PDS 16 KB  (zeroed by the flat image, like every
 *                        deCONZ factory image; deCONZ NVRAM is not stored here)
 *   0x09200-0x3EFFF  code + RO
 *   0x3EF00-0x3FFFF  NVRAM 4 KB (deCONZ NVRAM; image must END BEFORE HERE —
 *                        enforced by targets/cb2/image_guard.cmake)
 *
 * The post-mortem scratch row (hal/samr21/samr21_nvram.c) is 0x3E000:
 * inside the range above, but it must not hold *executing* code, because
 * every scratchpad mark erases that row — keep the image end below
 * 0x3E000 (the size guard only enforces 0x3EF00).
 *
 * RAM: 32 KB at 0x20000000; SP starts at the top.
 */

ENTRY(Reset_Handler)

MEMORY
{
  VECT  (rx)  : ORIGIN = 0x00005000, LENGTH = 0x100
  FLASH (rx)  : ORIGIN = 0x00009200, LENGTH = 0x3EF00 - 0x00009200
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 0x8000
}

SECTIONS
{
  /* Cortex-M0+ vector table: first two words full (SP, reset), the rest
   * 16-bit offsets within 128 bytes of the table start (startup provides
   * the thunks immediately after the table). */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP (*(.isr_vector));
    . = ALIGN(4);
  } > VECT

  .text :
  {
    . = ALIGN(4);
    *(.text);
    *(.text.*);
    *(.glue_7);
    *(.glue_7t);
    *(.rodata);
    *(.rodata.*);
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  /* Initialized data: stored in flash, copied to RAM by the startup. */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data);
    *(.data.*);
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH
  _data_load = LOADADDR(.data);

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss);
    *(.bss.*);
    *(COMMON);
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  /* Sanity: everything that runs must fit before the NVRAM. */
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}

ASSERT(_edata <= ORIGIN(RAM) + LENGTH(RAM), "RAM overflow");

/* Layout invariant: nothing may occupy the update-flag row 0x4F00..0x4FFF -
 * Nvram_ClearUpdateFlag() (hal/samr21/samr21_nvram.c) erases and rewrites that whole row at every
 * boot, and code fetched from a row being erased hangs the core. FLASH starts at 0x5000, just above
 * the row, and the ASSERTs make a future origin change fail the link instead of the board. Keep the
 * image end below the post-mortem row at 0x3E000 too: the size guard only enforces 0x3EF00. */
ASSERT((LOADADDR(.text) + SIZEOF(.text) <= 0x00004F00)
       || LOADADDR(.text) >= 0x00005000,
       ".text overlaps the update-flag row 0x4F00..0x4FFF (erased at every boot)");
ASSERT((LOADADDR(.data) + SIZEOF(.data) <= 0x00004F00)
       || LOADADDR(.data) >= 0x00005000,
       ".data flash copy overlaps the update-flag row 0x4F00..0x4FFF");
