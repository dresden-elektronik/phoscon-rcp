/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* RV32 startup for an ESP32 802.15.4 app image, entered by the board's ESP-IDF second-stage bootloader
 * (docs/hardware_esp32.md §12.3).
 *
 * The bootloader has loaded the RAM segments - .iram.text and .data - and mapped the two flash segments
 * before it calls the image's entry point, so there is no .data to copy; HAL_PreMain clears .bss. It
 * arrives here on the bootloader's own stack, which sits in memory the ROM still owns, and with its own
 * trap vector in mtvec, so the first three things are interrupts off, this image's stack and this
 * image's vectors.
 *
 * The core takes traps in vectored mode only (ESP32-C6 TRM 1.6.2): an exception enters at the table's
 * base, CPU interrupt n at base + 4n, and the base is 256-byte aligned. Every interrupt goes through one
 * entry that saves the caller-saved registers and hands mcause to Esp32Irq_Dispatch
 * (hal/esp32/esp32_irq.c); an exception hands mcause, mepc and mtval to Esp32Trap_Exception
 * (hal/esp32/esp32_panic.c), which does not return. No interrupt nests: the core clears MIE on entry and
 * mret restores it.
 *
 * Besides the four core instructions this is the only assembly of the ESP32 layer
 * (docs/coding-rules.md §1): mie and mtvec are set here, once, so C never writes a CSR. */

  .section .iram.text.entry, "ax"
  .global _start
  .type _start, @function
_start:
  csrci mstatus, 8
  csrr a0, mstatus

  .option push
  .option norelax
  la gp, __global_pointer$
  .option pop
  la sp, _stack_top

  la t0, esp32Vectors
  ori t0, t0, 1
  csrw mtvec, t0

  /* Unmask every external CPU interrupt at the core: which of them is live is the interrupt
   * controller's enable register, written by esp32_irq.c. Lines 0, 3, 4 and 7 are the core-local
   * sources, which this image does not use (TRM 1.6.1). */
  li t0, 0xFFFFFF66
  csrw mie, t0

  /* The bring-up witness of the state the bootloader handed over (hal/esp32/hal_esp32.c); it lives in
   * .data, which HAL_PreMain does not clear. */
  la t0, esp32BootMstatus
  sw a0, 0(t0)

  call HAL_PreMain
1:
  wfi
  j 1b
  .size _start, . - _start

/* ------------------------------------------------------------------ */
/* Trap vector table: entry 0 the exceptions, 1..31 the CPU lines     */
/* ------------------------------------------------------------------ */
  .section .iram.text.vectors, "ax"
  /* Without relaxation: each entry must stay one 4-byte jump at its offset, and the alignment is the
   * section's, not padding the linker would trim. */
  .option push
  .option norelax
  .p2align 8
  .global esp32Vectors
esp32Vectors:
  j esp32TrapException
  .rept 31
  j esp32TrapIrq
  .endr
  .option pop

/* The caller-saved registers, the ones a C function may clobber: ra, t0..t6, a0..a7. */
  .equ FRAME, 64

  .section .iram.text.trap, "ax"
  .type esp32TrapIrq, @function
esp32TrapIrq:
  addi sp, sp, -FRAME
  sw ra, 0(sp)
  sw t0, 4(sp)
  sw t1, 8(sp)
  sw t2, 12(sp)
  sw a0, 16(sp)
  sw a1, 20(sp)
  sw a2, 24(sp)
  sw a3, 28(sp)
  sw a4, 32(sp)
  sw a5, 36(sp)
  sw a6, 40(sp)
  sw a7, 44(sp)
  sw t3, 48(sp)
  sw t4, 52(sp)
  sw t5, 56(sp)
  sw t6, 60(sp)

  csrr a0, mcause
  call Esp32Irq_Dispatch

  lw ra, 0(sp)
  lw t0, 4(sp)
  lw t1, 8(sp)
  lw t2, 12(sp)
  lw a0, 16(sp)
  lw a1, 20(sp)
  lw a2, 24(sp)
  lw a3, 28(sp)
  lw a4, 32(sp)
  lw a5, 36(sp)
  lw a6, 40(sp)
  lw a7, 44(sp)
  lw t3, 48(sp)
  lw t4, 52(sp)
  lw t5, 56(sp)
  lw t6, 60(sp)
  addi sp, sp, FRAME
  mret
  .size esp32TrapIrq, . - esp32TrapIrq

  .type esp32TrapException, @function
esp32TrapException:
  csrr a0, mcause
  csrr a1, mepc
  csrr a2, mtval
  call Esp32Trap_Exception
2:
  j 2b
  .size esp32TrapException, . - esp32TrapException
