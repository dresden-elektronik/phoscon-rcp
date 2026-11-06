/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Cortex-M0+ startup for the CB2 app image (vector table at 0x5000).
 *
 * 32-bit entries throughout (the ARMv6-M vector format used by the
 * vendor bootloader's DeviceVectors table), so handlers may live
 * anywhere in flash.
 *
 * IRQ map (SAMR21, 28 peripherals): the radio's IRQ pin (EIC, IRQ 4), the host link's
 * vector (USB IRQ 7 on a CDC board, SERCOM5 IRQ 14 on a USART board) and SysTick (core 15)
 * get handlers; everything else falls into the default handler (panic — the WDT recovers
 * the chip). Both link vectors are named here whatever the board is; the one this board
 * does not drive is the weak default in samr21_panic.c, which the link's own driver
 * overrides.
 */

  .syntax unified
  .cpu cortex-m0plus
  .thumb

/* ------------------------------------------------------------------ */
/* Vector table (0x5000) — 32-bit entries, the ARMv6-M full format  */
/* (same layout as the vendor bootloader's DeviceVectors table).    */
/* ------------------------------------------------------------------ */
  .section .isr_vector, "ax"
  .global __isr_vector
__isr_vector:
  .long _estack                                /* 0: initial SP        */
  .long Reset_Handler                          /* 1: reset             */
  .long def_handler        /* NMI           */
  .long def_handler        /* HardFault     */
  .long 0                  /* reserved      */
  .long 0                  /* reserved      */
  .long 0                  /* reserved      */
  .long 0                  /* reserved      */
  .long 0                  /* reserved      */
  .long 0                  /* reserved      */
  .long 0                  /* reserved      */
  .long def_handler        /* SVCall        */
  .long def_handler        /* DebugMonitor  */
  .long 0                  /* reserved      */
  .long def_handler        /* PendSV        */
  .long SysTick_Handler    /* SysTick       */
  .long def_handler        /* 0: PM           */
  .long def_handler        /* 1: SYSCTRL      */
  .long def_handler        /* 2: WDT          */
  .long def_handler        /* 3: RTC          */
  .long EIC_Handler        /* 4: EIC          */
  .long def_handler        /* 5: NVMCTRL      */
  .long def_handler        /* 6: DMAC         */
  .long USB_Handler        /* 7: USB          */
  .long def_handler        /* 8: EVSYS        */
  .long def_handler        /* 9: SERCOM0      */
  .long def_handler        /* 10: SERCOM1      */
  .long def_handler        /* 11: SERCOM2      */
  .long def_handler        /* 12: SERCOM3      */
  .long def_handler        /* 13: SERCOM4      */
  .long SERCOM5_Handler    /* 14: SERCOM5      */
  .long def_handler        /* 15: TCC0         */
  .long def_handler        /* 16: TCC1         */
  .long def_handler        /* 17: TCC2         */
  .long def_handler        /* 18: TC3          */
  .long def_handler        /* 19: TC4          */
  .long def_handler        /* 20: TC5          */
  .long def_handler        /* 21: reserved21   */
  .long def_handler        /* 22: reserved22   */
  .long def_handler        /* 23: ADC          */
  .long def_handler        /* 24: AC           */
  .long def_handler        /* 25: reserved25   */
  .long def_handler        /* 26: PTC          */
  .long def_handler        /* 27: reserved27   */
  .long def_handler        /* 28: reserved28   */

/* ------------------------------------------------------------------ */
/* Reset handler                                                       */
/* ------------------------------------------------------------------ */
  .section .text.startup
  .thumb_func
  .global Reset_Handler
Reset_Handler:
  /* SP already set from vector 0. */
  bl HAL_PreMain
1:  b 1b
