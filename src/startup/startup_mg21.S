/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Cortex-M33 startup for the ConBee III application image (vector table at the board's app
 * origin, 0x4000 below the bootloader).
 *
 * 77 entries: 16 core exceptions plus IRQ numbers 0..60, the highest being M33CTI1
 * (docs/hardware_mg21.md §10.1). test/unit/test_mg21_vectors.py checks this table entry by entry
 * against a record generated from the part's own interrupt-number enum, because a vector written
 * in the wrong slot hands one interrupt to another driver's handler and nothing on the board says
 * so.
 *
 * Three entries have handlers of their own below the core: the two USART0 vectors of the host link
 * (hal/mg21/mg21_uart.c) and the millisecond tick (hal/mg21/mg21_clock.c). The fault vectors get
 * named handlers so a panic can say which fault it is reporting (hal/mg21/mg21_panic.c). The ten radio
 * slots, AGC through SYNTH, are named too, weakly: they default to def_handler and a build whose radio
 * back-end owns radio interrupts (hal/mg21/rail/mg21_rail.c) defines them. Everything else is
 * def_handler, which reports an unexpected exception and lets the watchdog land the board in the
 * bootloader window (docs/hardware_mg21.md §10.7).
 *
 * Slot 7 is ARMv8-M's SecureFault. The part's own enum does not name it, so this table hands it to
 * def_handler as a reserved entry and says so rather than inventing a number for it.
 */

  .syntax unified
  .cpu cortex-m33
  .thumb

/* ------------------------------------------------------------------ */
/* Vector table (app origin) - 77 entries (0x134), padded to 512      */
/* ------------------------------------------------------------------ */
  .section .isr_vector, "ax"
  .global __isr_vector
__isr_vector:
  .long _estack                  /* 0: initial stack pointer      */
  .long Reset_Handler            /* 1: reset                      */
  .long NMI_Handler              /* 2: NMI                        */
  .long HardFault_Handler        /* 3: hard fault                 */
  .long MemManage_Handler        /* 4: memory management fault    */
  .long BusFault_Handler         /* 5: bus fault                  */
  .long UsageFault_Handler       /* 6: usage fault                */
  .long def_handler              /* 7: SecureFault, unnamed here*/
  .long def_handler              /* 8:  reserved                  */
  .long def_handler              /* 9:  reserved                  */
  .long def_handler              /* 10: reserved                  */
  .long SVCall_Handler           /* 11: SVC                       */
  .long DebugMonitor_Handler     /* 12: monitor                   */
  .long def_handler              /* 13: reserved                  */
  .long def_handler              /* 14: PendSV, unused here       */
  .long SysTick_Handler          /* 15: the board's millisecond   */

  .long def_handler              /* 0:  SETAMPERHOST              */
  .long def_handler              /* 1:  SEMBRX                    */
  .long def_handler              /* 2:  SEMBTX                    */
  .long def_handler              /* 3:  SMU_SECURE                */
  .long def_handler              /* 4:  SMU_PRIVILEGED            */
  .long def_handler              /* 5:  EMU                       */
  .long def_handler              /* 6:  TIMER0                    */
  .long def_handler              /* 7:  TIMER1                    */
  .long def_handler              /* 8:  TIMER2                    */
  .long def_handler              /* 9:  TIMER3                    */
  .long def_handler              /* 10: RTCC                      */
  .long USART0_RX_Handler        /* 11: the host link's receive   */
  .long USART0_TX_Handler        /* 12: the host link's transmit  */
  .long def_handler              /* 13: USART1_RX                 */
  .long def_handler              /* 14: USART1_TX                 */
  .long def_handler              /* 15: USART2_RX                 */
  .long def_handler              /* 16: USART2_TX                 */
  .long def_handler              /* 17: ICACHE0                   */
  .long def_handler              /* 18: BURTC                     */
  .long def_handler              /* 19: LETIMER0                  */
  .long def_handler              /* 20: SYSCFG                    */
  .long def_handler              /* 21: LDMA                      */
  .long def_handler              /* 22: LFXO                      */
  .long def_handler              /* 23: LFRCO                     */
  .long def_handler              /* 24: ULFRCO                    */
  .long def_handler              /* 25: GPIO_ODD                  */
  .long def_handler              /* 26: GPIO_EVEN                 */
  .long def_handler              /* 27: I2C0                      */
  .long def_handler              /* 28: I2C1                      */
  .long def_handler              /* 29: EMUDG                     */
  .long def_handler              /* 30: EMUSE                     */
  .long AGC_Handler              /* 31: AGC                   */
  .long BUFC_Handler             /* 32: BUFC                  */
  .long FRC_PRI_Handler          /* 33: FRC_PRI               */
  .long FRC_Handler              /* 34: FRC                   */
  .long MODEM_Handler            /* 35: MODEM                 */
  .long PROTIMER_Handler         /* 36: PROTIMER              */
  .long RAC_RSM_Handler          /* 37: RAC_RSM               */
  .long RAC_SEQ_Handler          /* 38: RAC_SEQ               */
  .long PRORTC_Handler           /* 39: PRORTC                */
  .long SYNTH_Handler            /* 40: SYNTH                 */
  .long def_handler              /* 41: ACMP0                     */
  .long def_handler              /* 42: ACMP1                     */
  .long def_handler              /* 43: WDOG0                     */
  .long def_handler              /* 44: WDOG1                     */
  .long def_handler              /* 45: HFXO00                    */
  .long def_handler              /* 46: HFRCO0                    */
  .long def_handler              /* 47: HFRCOEM23                 */
  .long def_handler              /* 48: CMU                       */
  .long def_handler              /* 49: AES                       */
  .long def_handler              /* 50: IADC                      */
  .long def_handler              /* 51: MSC                       */
  .long def_handler              /* 52: DPLL0                     */
  .long def_handler              /* 53: SW0                       */
  .long def_handler              /* 54: SW1                       */
  .long def_handler              /* 55: SW2                       */
  .long def_handler              /* 56: SW3                       */
  .long def_handler              /* 57: KERNEL0                   */
  .long def_handler              /* 58: KERNEL1                   */
  .long def_handler              /* 59: M33CTI0                   */
  .long def_handler              /* 60: M33CTI1                   */
  /* Pad the table to 512 bytes so its base (VTOR) is aligned to its own size: the Cortex-M requires
   * the vector table to be aligned to the size of the table, and the 77 real entries (0x134) would
   * leave 0x4000 misaligned. The pad is unreachable vector space, not entries - the part's highest
   * IRQ is 60, the last real vector above. */
  .space 512 - ( . - __isr_vector )

/* ------------------------------------------------------------------ */
/* The radio's vectors, weak: one shared stub into def_handler          */
/* ------------------------------------------------------------------ */
  .section .text.radio_vectors, "ax"
  .thumb_func
  .weak AGC_Handler
AGC_Handler:
  b.w def_handler
  .weak BUFC_Handler
  .thumb_set BUFC_Handler, AGC_Handler
  .weak FRC_PRI_Handler
  .thumb_set FRC_PRI_Handler, AGC_Handler
  .weak FRC_Handler
  .thumb_set FRC_Handler, AGC_Handler
  .weak MODEM_Handler
  .thumb_set MODEM_Handler, AGC_Handler
  .weak PROTIMER_Handler
  .thumb_set PROTIMER_Handler, AGC_Handler
  .weak RAC_RSM_Handler
  .thumb_set RAC_RSM_Handler, AGC_Handler
  .weak RAC_SEQ_Handler
  .thumb_set RAC_SEQ_Handler, AGC_Handler
  .weak PRORTC_Handler
  .thumb_set PRORTC_Handler, AGC_Handler
  .weak SYNTH_Handler
  .thumb_set SYNTH_Handler, AGC_Handler

/* ------------------------------------------------------------------ */
/* Reset handler                                                       */
/* ------------------------------------------------------------------ */
  .section .text.startup
  .thumb_func
  .global Reset_Handler
Reset_Handler:
  /* SP and VTOR come from the table the bootloader jumped into; HAL_PreMain points VTOR at the
   * vector table anyway, and clears .bss and copies .data before anything else runs. */
  bl HAL_PreMain
1:  b 1b
