/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* ConBee III (EFR32MG21A010F1024IM32) application image.
 *
 * The layout is the deSiLabs bootloader's, computed in targets/cb3/board.cmake from the part's size
 * and page size and asserted there (docs/hardware_mg21.md §10.7):
 *
 *   0x00000-0x03FFF  BTL sector  (the vendor bootloader; never written by a GCF flash of the app)
 *   0x04000-0x7BFFF  AppSection  (480 KiB): our image, then the 16-byte AppInfo trailer at 0x7BFF0
 *   0x07C000-0xF7FFF ImageSector (496 KiB): the bootloader's staging area, erased after every install
 *   0x0F8000-0xFFFFF NVM3        (32 KiB): the application's own storage
 *
 * The app is linked into the AppSection and must END BELOW 0x7BFF0: the bootloader writes its AppInfo
 * trailer there after the app, and an image that reaches it fails the verification the trailer feeds.
 * The vendor's own linker script allows 0x4000 + 0xFC000 (everything past the bootloader), which is
 * laxer than what this board's bootloader can actually install; targets/cb3/image_guard.cmake enforces
 * the real limit (docs/build-and-flash.md §9).
 *
 * RAM: 96 KiB at 0x20000000, SP starts at the top. The first 32 bytes of it are the bootloader's reset
 * record, so they are a region of their own and usable RAM begins at 0x20000020 (docs/hardware_mg21.md §10.7).
 */

ENTRY(Reset_Handler)

MEMORY
{
  VECT  (rx)  : ORIGIN = 0x00004000, LENGTH = 0x200
  FLASH (rx)  : ORIGIN = 0x00004200, LENGTH = 0x7BFF0 - 0x00004200

  /* The reset record the bootloader leaves at 0x20000000 (docs/hardware_mg21.md §10.7): 16 u16 that say why this
   * boot happened, and that an image writes to ask for the bootloader. It gets a region of its own so
   * that .data and .bss cannot be placed over it: the bootloader reads it, jumps, and the application
   * is the next thing that touches that address. */
  RECORD (xrw) : ORIGIN = 0x20000000, LENGTH = 0x20
  RAM   (xrw) : ORIGIN = 0x20000020, LENGTH = 0x18000 - 0x20

  /* The panic mark a soft reset must survive lives here, in its own region, because the bootloader's
   * crt0 clears its own .bss on every boot - memset(0x200001EC, 0, 0xA60), i.e. it zeroes RAM
   * 0x200001EC-0x20000C4B before it jumps to the app (deSiLabsBootloader.axf Reset_Handler, the
   * literal pool at 0x1c40/0x1c44). A mark placed in the ordinary .bss/.noinit flow lands where .bss
   * ends, which for the small probe image is inside that window and is wiped before the next boot can
   * read it; only a build whose .bss happens to be large enough escapes by luck. 0x20008000 sits
   * above the clear and above the largest .bss either image produces, with the asserts below keeping
   * it so (docs/hardware_mg21.md §10.7). */
  NOINIT (xrw) : ORIGIN = 0x20008000, LENGTH = 0x100
}

SECTIONS
{
  /* Cortex-M33 vector table: the MG21 table is 77 entries — 16 system plus IRQ numbers 0..60, the
   * highest being M33CTI1 (SDK part header) — so 0x134 bytes, which is what the vendor image's
   * .vectors section measures. 0x200 leaves room for the handler thunks the startup puts after it. */
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
    *(.rodata);
    *(.rodata.*);
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  /* The record is read, not initialised: NOLOAD, KEEP so a section attribute names it. */
  .reset_record (NOLOAD) :
  {
    . = ALIGN(4);
    KEEP (*(.reset_record));
    . = ALIGN(4);
  } > RECORD

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

  /* SRAM a soft reset leaves alone: the mark a panic writes before it resets, read back by the next
   * boot (hal/mg21/mg21_panic.c). Nothing initialises it, so its own signature is what makes it
   * mean something. It is placed in NOINIT, above the bootloader's .bss clear, not in the RAM flow -
   * see the NOINIT region. */
  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    _snoinit = .;
    KEEP (*(.noinit));
    . = ALIGN(4);
    _enoinit = .;
  } > NOINIT

  /* SP top, the same convention as the SAMR21 scripts. */
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}

/* The image may not reach the AppInfo trailer. The linker catches a section overflow; the size guard
 * catches a flat binary that fits the region but crosses the bound. */
ASSERT(_edata <= ORIGIN(RAM) + LENGTH(RAM), "cb3: RAM overflow (96 KiB at 0x20000000)")
ASSERT(ORIGIN(RECORD) == 0x20000000, "cb3: the reset record region is not at the address the bootloader uses")
ASSERT(LENGTH(RECORD) == 32, "cb3: the reset record region is not 16 words")
ASSERT(ORIGIN(RAM) == ORIGIN(RECORD) + LENGTH(RECORD), "cb3: RAM must start above the reset record")

/* The Cortex-M requires the vector table to be aligned to its own size. The startup pads the 77
 * real entries to 512 bytes, so the base (VTOR) must be 512-aligned and the table must start at the
 * VECT origin. */
ASSERT(ORIGIN(VECT) % 0x200 == 0, "cb3: the vector table base must be aligned to its 512-byte padded size")
ASSERT(__isr_vector == ORIGIN(VECT), "cb3: the vector table must start at the VECT origin")

/* The panic mark must survive the bootloader's crt0 .bss clear (0x200001EC + 0xA60), and the RAM
 * sections must not grow into or out of the NOINIT region that holds it. The mark is read back after
 * a soft reset, so it must also sit clear of the stack, which grows down from _estack. */
ASSERT(ORIGIN(NOINIT) > 0x20000C4B, "cb3: the panic mark must sit above the bootloader's .bss clear (0x200001EC-0x20000C4B)")
ASSERT(_ebss <= ORIGIN(NOINIT), "cb3: .bss must not reach the panic-mark region")
ASSERT(ORIGIN(NOINIT) + LENGTH(NOINIT) + 0x2000 <= ORIGIN(RAM) + LENGTH(RAM), "cb3: leave at least 8 KiB of stack above the panic-mark region")
ASSERT(ORIGIN(RAM) + LENGTH(RAM) <= 0x20018000, "cb3: RAM must not exceed the 96 KiB the part has")
