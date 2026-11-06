/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_regs_ref.h - generated, do not edit: tools/mg21_regs_gold.py compiles
 * tools/mg21_regs_gold.c against the vendor EFR32MG21 device headers
 * (Simplicity SDK 2024.12.1, Silicon Labs, Zlib). The names are the
 * vendor's own, prefixed REF_. test/unit/test_mg21_regs.c compares
 * src/hal/mg21/mg21_regs.h against this record. */
#ifndef MG21_REGS_REF_H
#define MG21_REGS_REF_H

#define REF_BASE_EMU                          0x50004000u
#define REF_BASE_CMU                          0x50008000u
#define REF_BASE_HFXO0                        0x5000C000u
#define REF_BASE_HFRCO0                       0x50010000u
#define REF_BASE_FSRCO                        0x50018000u
#define REF_BASE_DPLL0                        0x5001C000u
#define REF_BASE_LFRCO                        0x50024000u
#define REF_BASE_MSC                          0x50030000u
#define REF_BASE_GPIO                         0x5003C000u
#define REF_BASE_LDMA                         0x50040000u
#define REF_BASE_USART0                       0x50058000u
#define REF_BASE_WDOG0                        0x5A018000u
#define REF_BASE_SYSCFG                       0x5007C000u
#define REF_FLD_CORE_SCB_BASE                            0xE000ED00u	/* vendor CORE_SCB_BASE */
#define REF_FLD_CORE_SYSTICK_BASE                        0xE000E010u	/* vendor CORE_SYSTICK_BASE */
#define REF_FLD_CORE_DWT_BASE                            0xE0001000u	/* vendor CORE_DWT_BASE */
#define REF_FLD_CORE_SCB_VTOR                            0xE000ED08u	/* vendor CORE_SCB_VTOR */
#define REF_FLD_CORE_SCB_AIRCR                           0xE000ED0Cu	/* vendor CORE_SCB_AIRCR */
#define REF_FLD_CORE_SCB_SHCSR                           0xE000ED24u	/* vendor CORE_SCB_SHCSR */
#define REF_FLD_CORE_SCB_DEMCR                           0xE000EDFCu	/* vendor CORE_SCB_DEMCR */
#define REF_FLD_CORE_SYSTICK_CTRL                        0xE000E010u	/* vendor CORE_SYSTICK_CTRL */
#define REF_FLD_CORE_SYSTICK_LOAD                        0xE000E014u	/* vendor CORE_SYSTICK_LOAD */
#define REF_FLD_CORE_SYSTICK_VAL                         0xE000E018u	/* vendor CORE_SYSTICK_VAL */
#define REF_FLD_CORE_SYSTICK_CALIB                       0xE000E01Cu	/* vendor CORE_SYSTICK_CALIB */
#define REF_FLD_CORE_DWT_CTRL                            0xE0001000u	/* vendor CORE_DWT_CTRL */
#define REF_FLD_CORE_DWT_CYCCNT                          0xE0001004u	/* vendor CORE_DWT_CYCCNT */
#define REF_FLD_CORE_NVIC_ISER0                          0xE000E100u	/* vendor CORE_NVIC_ISER0 */
#define REF_FLD_CORE_NVIC_ICER0                          0xE000E180u	/* vendor CORE_NVIC_ICER0 */
#define REF_FLD_CORE_NVIC_ICER1                          0xE000E184u	/* vendor CORE_NVIC_ICER1 */
#define REF_FLD_CORE_NVIC_ICPR0                          0xE000E280u	/* vendor CORE_NVIC_ICPR0 */
#define REF_FLD_CORE_NVIC_ICPR1                          0xE000E284u	/* vendor CORE_NVIC_ICPR1 */
#define REF_FLD_CORE_NVIC_IPR0                           0xE000E400u	/* vendor CORE_NVIC_IPR0 */
#define REF_FLD_CORE_SCB_AIRCR_VECTKEY_POS               0x10u	/* vendor CORE_SCB_AIRCR_VECTKEY_POS */
#define REF_FLD_CORE_SCB_AIRCR_VECTKEY_MASK              0xFFFF0000u	/* vendor CORE_SCB_AIRCR_VECTKEY_MASK */
#define REF_FLD_CORE_SCB_AIRCR_SYSRESETREQ               0x4u	/* vendor CORE_SCB_AIRCR_SYSRESETREQ */
#define REF_FLD_CORE_SCB_SHCSR_MEMFAULTENA               0x10000u	/* vendor CORE_SCB_SHCSR_MEMFAULTENA */
#define REF_FLD_CORE_SCB_SHCSR_BUSFAULTENA               0x20000u	/* vendor CORE_SCB_SHCSR_BUSFAULTENA */
#define REF_FLD_CORE_SCB_SHCSR_USGFAULTENA               0x40000u	/* vendor CORE_SCB_SHCSR_USGFAULTENA */
#define REF_FLD_CORE_SYSTICK_CTRL_ENABLE                 0x1u	/* vendor CORE_SYSTICK_CTRL_ENABLE */
#define REF_FLD_CORE_SYSTICK_CTRL_TICKINT                0x2u	/* vendor CORE_SYSTICK_CTRL_TICKINT */
#define REF_FLD_CORE_SYSTICK_CTRL_CLKSOURCE              0x4u	/* vendor CORE_SYSTICK_CTRL_CLKSOURCE */
#define REF_FLD_CORE_SYSTICK_CTRL_COUNTFLAG              0x10000u	/* vendor CORE_SYSTICK_CTRL_COUNTFLAG */
#define REF_FLD_CORE_SYSTICK_LOAD_RELOAD                 0xFFFFFFu	/* vendor CORE_SYSTICK_LOAD_RELOAD */
#define REF_FLD_CORE_DWT_CTRL_CYCCNTENA                  0x1u	/* vendor CORE_DWT_CTRL_CYCCNTENA */
#define REF_FLD_CORE_DEMCR_TRCENA                        0x1000000u	/* vendor CORE_DEMCR_TRCENA */
#define REF_BASE_SMU_S                        0x44008000u
#define REF_OFF_SMU_PPUSATD0                 0x60u
#define REF_OFF_SMU_PPUSATD1                 0x64u
#define REF_OFF_SMU_PPUSATD0_SET             0x1060u
#define REF_OFF_SMU_PPUSATD0_CLR             0x2060u
#define REF_FLD_SMU_PPUSATD0_MASK                        0xFFFFFFFEu	/* vendor _SMU_PPUSATD0_MASK */
#define REF_FLD_SMU_PPUSATD1_MASK                        0x3FFFu	/* vendor _SMU_PPUSATD1_MASK */
#define REF_FLD_SMU_PPUSATD1_SMU                         0x800u	/* vendor SMU_PPUSATD1_SMU */
#define REF_FLD_SMU_PPUSATD1_SEMAILBOX                   0x2000u	/* vendor SMU_PPUSATD1_SEMAILBOX */
#define REF_IRQ_USART0_RX                                11
#define REF_IRQ_USART0_TX                                12
#define REF_IRQ_WDOG0                                    43
#define REF_IRQ_SYSTICK                                  -1
#define REF_IRQ_PENDSV                                   -2
#define REF_IRQ_SVCALL                                   -5
#define REF_IRQ_HARDFAULT                                -13
#define REF_FLD_FLASH_SIZE                               0x100000u	/* vendor FLASH_SIZE */
#define REF_FLD_FLASH_PAGE_SIZE                          0x2000u	/* vendor FLASH_PAGE_SIZE */
#define REF_FLD_USERDATA_BASE                            0xFE00000u	/* vendor USERDATA_BASE */
#define REF_FLD_USERDATA_SIZE                            0x400u	/* vendor USERDATA_SIZE */
#define REF_FLD_NVIC_PRIO_BITS                           0x4u	/* vendor NVIC_PRIO_BITS */

/* register offsets, as the vendor typedefs lay them out */
#define REF_OFF_CMU_IPVERSION                0x00u
#define REF_OFF_CMU_STATUS                   0x08u
#define REF_OFF_CMU_LOCK                     0x10u
#define REF_OFF_CMU_WDOGLOCK                 0x14u
#define REF_OFF_CMU_IF                       0x20u
#define REF_OFF_CMU_IEN                      0x24u
#define REF_OFF_CMU_CALCMD                   0x50u
#define REF_OFF_CMU_CALCTRL                  0x54u
#define REF_OFF_CMU_CALCNT                   0x58u
#define REF_OFF_CMU_SYSCLKCTRL               0x70u
#define REF_OFF_CMU_TRACECLKCTRL             0x80u
#define REF_OFF_CMU_EXPORTCLKCTRL            0x90u
#define REF_OFF_CMU_EM01GRPACLKCTRL          0x120u
#define REF_OFF_CMU_EM23GRPACLKCTRL          0x140u
#define REF_OFF_CMU_WDOG0CLKCTRL             0x200u
#define REF_OFF_CMU_RADIOCLKCTRL             0x280u
#define REF_OFF_CMU_RADIOCLKCTRL             0x280u
#define REF_OFF_HFXO0_XTALCFG                  0x10u
#define REF_OFF_HFXO0_XTALCTRL                 0x18u
#define REF_OFF_HFXO0_CFG                      0x20u
#define REF_OFF_HFXO0_CTRL                     0x28u
#define REF_OFF_HFXO0_CMD                      0x50u
#define REF_OFF_HFXO0_STATUS                   0x58u
#define REF_OFF_HFXO0_IF                       0x70u
#define REF_OFF_HFXO0_IEN                      0x74u
#define REF_OFF_HFXO0_LOCK                     0x80u
#define REF_OFF_LFRCO_STATUS                   0x08u
#define REF_OFF_LFRCO_CAL                      0x0Cu
#define REF_OFF_LFRCO_IF                       0x14u
#define REF_OFF_LFRCO_IEN                      0x18u
#define REF_OFF_LFRCO_SYNCBUSY                 0x1Cu
#define REF_OFF_LFRCO_LOCK                     0x20u
#define REF_OFF_WDOG0_IPVERSION                0x00u
#define REF_OFF_WDOG0_EN                       0x04u
#define REF_OFF_WDOG0_CFG                      0x08u
#define REF_OFF_WDOG0_CMD                      0x0Cu
#define REF_OFF_WDOG0_STATUS                   0x14u
#define REF_OFF_WDOG0_IF                       0x18u
#define REF_OFF_WDOG0_IEN                      0x1Cu
#define REF_OFF_WDOG0_LOCK                     0x20u
#define REF_OFF_WDOG0_SYNCBUSY                 0x24u
#define REF_OFF_USART0_EN                       0x04u
#define REF_OFF_USART0_CTRL                     0x08u
#define REF_OFF_USART0_FRAME                    0x0Cu
#define REF_OFF_USART0_TRIGCTRL                 0x10u
#define REF_OFF_USART0_CMD                      0x14u
#define REF_OFF_USART0_STATUS                   0x18u
#define REF_OFF_USART0_CLKDIV                   0x1Cu
#define REF_OFF_USART0_RXDATAX                  0x20u
#define REF_OFF_USART0_RXDATA                   0x24u
#define REF_OFF_USART0_TXDATAX                  0x38u
#define REF_OFF_USART0_TXDATA                   0x3Cu
#define REF_OFF_USART0_IF                       0x48u
#define REF_OFF_USART0_IEN                      0x4Cu
#define REF_OFF_USART0_IF_CLR                   0x2048u
#define REF_OFF_SYSCFG_CHIPREVHW                0x10u
#define REF_OFF_SYSCFG_CHIPREV                  0x14u
#define REF_OFF_SYSCFG_CFGSYSTIC                0x24u
#define REF_OFF_GPIO_P0                       0x00u
#define REF_OFF_GPIO_P1                       0x30u
#define REF_OFF_GPIO_LOCK                     0x300u
#define REF_OFF_GPIO_USARTROUTE0              0x5ACu
#define REF_OFF_GPIO_USARTROUTE1              0x5CCu
#define REF_OFF_GPIOP_CTRL                     0x00u
#define REF_OFF_GPIOP_MODEL                    0x04u
#define REF_OFF_GPIOP_MODEH                    0x0Cu
#define REF_OFF_GPIOP_DOUT                     0x10u
#define REF_OFF_GPIOP_DIN                      0x14u
#define REF_OFF_USARTR_ROUTEEN                  0x00u
#define REF_OFF_USARTR_CSROUTE                  0x04u
#define REF_OFF_USARTR_CTSROUTE                 0x08u
#define REF_OFF_USARTR_RTSROUTE                 0x0Cu
#define REF_OFF_USARTR_RXROUTE                  0x10u
#define REF_OFF_USARTR_CLKROUTE                 0x14u
#define REF_OFF_USARTR_TXROUTE                  0x18u
#define REF_FLD_GPIO_PORT_STRIDE                         0x30u	/* vendor GPIO_PORT_STRIDE */
#define REF_FLD_GPIO_USARTROUTE_STRIDE                   0x20u	/* vendor GPIO_USARTROUTE_STRIDE */

/* field encodings, by the vendor's own names */
#define REF_FLD_CMU_SYSCLKCTRL_CLKSEL_SHIFT              0x0u	/* vendor _CMU_SYSCLKCTRL_CLKSEL_SHIFT */
#define REF_FLD_CMU_SYSCLKCTRL_CLKSEL_MASK               0x7u	/* vendor _CMU_SYSCLKCTRL_CLKSEL_MASK */
#define REF_FLD_CMU_SYSCLKCTRL_CLKSEL_DEFAULT            0x1u	/* vendor CMU_SYSCLKCTRL_CLKSEL_DEFAULT */
#define REF_FLD_CMU_SYSCLKCTRL_CLKSEL_FSRCO              0x1u	/* vendor CMU_SYSCLKCTRL_CLKSEL_FSRCO */
#define REF_FLD_CMU_SYSCLKCTRL_CLKSEL_HFRCODPLL          0x2u	/* vendor CMU_SYSCLKCTRL_CLKSEL_HFRCODPLL */
#define REF_FLD_CMU_SYSCLKCTRL_CLKSEL_HFXO               0x3u	/* vendor CMU_SYSCLKCTRL_CLKSEL_HFXO */
#define REF_FLD_CMU_SYSCLKCTRL_HCLKPRESC_SHIFT           0xCu	/* vendor _CMU_SYSCLKCTRL_HCLKPRESC_SHIFT */
#define REF_FLD_CMU_SYSCLKCTRL_HCLKPRESC_MASK            0x3000u	/* vendor _CMU_SYSCLKCTRL_HCLKPRESC_MASK */
#define REF_FLD_CMU_SYSCLKCTRL_HCLKPRESC_DEFAULT         0x0u	/* vendor CMU_SYSCLKCTRL_HCLKPRESC_DEFAULT */
#define REF_FLD_CMU_SYSCLKCTRL_PCLKPRESC_SHIFT           0xAu	/* vendor _CMU_SYSCLKCTRL_PCLKPRESC_SHIFT */
#define REF_FLD_CMU_SYSCLKCTRL_PCLKPRESC_MASK            0x400u	/* vendor _CMU_SYSCLKCTRL_PCLKPRESC_MASK */
#define REF_FLD_CMU_SYSCLKCTRL_PCLKPRESC_DEFAULT         0x0u	/* vendor CMU_SYSCLKCTRL_PCLKPRESC_DEFAULT */
#define REF_FLD_CMU_EM01GRPACLKCTRL_CLKSEL_SHIFT         0x0u	/* vendor _CMU_EM01GRPACLKCTRL_CLKSEL_SHIFT */
#define REF_FLD_CMU_EM01GRPACLKCTRL_CLKSEL_MASK          0x7u	/* vendor _CMU_EM01GRPACLKCTRL_CLKSEL_MASK */
#define REF_FLD_CMU_EM01GRPACLKCTRL_CLKSEL_HFRCODPLL     0x1u	/* vendor CMU_EM01GRPACLKCTRL_CLKSEL_HFRCODPLL */
#define REF_FLD_CMU_EM01GRPACLKCTRL_CLKSEL_HFXO          0x2u	/* vendor CMU_EM01GRPACLKCTRL_CLKSEL_HFXO */
#define REF_FLD_CMU_EM01GRPACLKCTRL_CLKSEL_FSRCO         0x4u	/* vendor CMU_EM01GRPACLKCTRL_CLKSEL_FSRCO */
#define REF_FLD_CMU_WDOG0CLKCTRL_CLKSEL_SHIFT            0x0u	/* vendor _CMU_WDOG0CLKCTRL_CLKSEL_SHIFT */
#define REF_FLD_CMU_WDOG0CLKCTRL_CLKSEL_MASK             0x7u	/* vendor _CMU_WDOG0CLKCTRL_CLKSEL_MASK */
#define REF_FLD_CMU_WDOG0CLKCTRL_CLKSEL_LFRCO            0x1u	/* vendor CMU_WDOG0CLKCTRL_CLKSEL_LFRCO */
#define REF_FLD_CMU_WDOG0CLKCTRL_CLKSEL_ULFRCO           0x3u	/* vendor CMU_WDOG0CLKCTRL_CLKSEL_ULFRCO */
#define REF_FLD_CMU_RADIOCLKCTRL_EN                      0x1u	/* vendor CMU_RADIOCLKCTRL_EN */
#define REF_FLD_CMU_EM23GRPACLKCTRL_CLKSEL_SHIFT         0x0u	/* vendor _CMU_EM23GRPACLKCTRL_CLKSEL_SHIFT */
#define REF_FLD_CMU_EM23GRPACLKCTRL_CLKSEL_MASK          0x3u	/* vendor _CMU_EM23GRPACLKCTRL_CLKSEL_MASK */
#define REF_FLD_CMU_EM23GRPACLKCTRL_CLKSEL_DEFAULT       0x1u	/* vendor CMU_EM23GRPACLKCTRL_CLKSEL_DEFAULT */
#define REF_FLD_CMU_EM23GRPACLKCTRL_CLKSEL_LFRCO         0x1u	/* vendor CMU_EM23GRPACLKCTRL_CLKSEL_LFRCO */
#define REF_FLD_CMU_EM23GRPACLKCTRL_CLKSEL_LFXO          0x2u	/* vendor CMU_EM23GRPACLKCTRL_CLKSEL_LFXO */
#define REF_FLD_CMU_EM23GRPACLKCTRL_CLKSEL_ULFRCO        0x3u	/* vendor CMU_EM23GRPACLKCTRL_CLKSEL_ULFRCO */
#define REF_FLD_CMU_EM01GRPACLKCTRL_CLKSEL_DEFAULT       0x1u	/* vendor CMU_EM01GRPACLKCTRL_CLKSEL_DEFAULT */
#define REF_FLD_CMU_LOCK_LOCKKEY_UNLOCK                  0x93F7u	/* vendor CMU_LOCK_LOCKKEY_UNLOCK */
#define REF_FLD_CMU_WDOGLOCK_LOCKKEY_UNLOCK              0x93F7u	/* vendor CMU_WDOGLOCK_LOCKKEY_UNLOCK */
#define REF_FLD_CMU_STATUS_CALRDY                        0x1u	/* vendor CMU_STATUS_CALRDY */
#define REF_FLD_CMU_CALCMD_CALSTART                      0x1u	/* vendor CMU_CALCMD_CALSTART */
#define REF_FLD_CMU_CALCTRL_CONT                         0x800000u	/* vendor CMU_CALCTRL_CONT */
#define REF_FLD_HFXO_CTRL_FORCEEN                        0x1u	/* vendor HFXO_CTRL_FORCEEN */
#define REF_FLD_HFXO_CTRL_DISONDEMAND                    0x2u	/* vendor HFXO_CTRL_DISONDEMAND */
#define REF_FLD_HFXO_CTRL_FORCEXI2GNDANA                 0x10u	/* vendor HFXO_CTRL_FORCEXI2GNDANA */
#define REF_FLD_HFXO_CTRL_FORCEXO2GNDANA                 0x20u	/* vendor HFXO_CTRL_FORCEXO2GNDANA */
#define REF_FLD_HFXO_STATUS_RDY                          0x1u	/* vendor HFXO_STATUS_RDY */
#define REF_FLD_HFXO_STATUS_ENS                          0x10000u	/* vendor HFXO_STATUS_ENS */
#define REF_FLD_HFXO_STATUS_HWREQ                        0x20000u	/* vendor HFXO_STATUS_HWREQ */
#define REF_FLD_HFXO_CMD_COREBIASOPT                     0x1u	/* vendor HFXO_CMD_COREBIASOPT */
#define REF_FLD_HFXO_IF_RDY                              0x1u	/* vendor HFXO_IF_RDY */
#define REF_FLD_HFXO_CFG_MODE                            0x1u	/* vendor HFXO_CFG_MODE */
#define REF_FLD_HFXO_LOCK_LOCKKEY_UNLOCK                 0x580Eu	/* vendor HFXO_LOCK_LOCKKEY_UNLOCK */
#define REF_FLD_LFRCO_CAL_MASK                           0xFFu	/* vendor _LFRCO_CAL_MASK */
#define REF_FLD_LFRCO_SYNCBUSY_MASK                      0x1u	/* vendor _LFRCO_SYNCBUSY_MASK */
#define REF_FLD_LFRCO_STATUS_RDY                         0x1u	/* vendor LFRCO_STATUS_RDY */
#define REF_FLD_LFRCO_STATUS_ENS                         0x10000u	/* vendor LFRCO_STATUS_ENS */
#define REF_FLD_WDOG_EN_EN                               0x1u	/* vendor WDOG_EN_EN */
#define REF_FLD_WDOG_CFG_MASK                            0x730F071Fu	/* vendor _WDOG_CFG_MASK */
#define REF_FLD_WDOG_CFG_PERSEL_SHIFT                    0x10u	/* vendor _WDOG_CFG_PERSEL_SHIFT */
#define REF_FLD_WDOG_CFG_PERSEL_MASK                     0xF0000u	/* vendor _WDOG_CFG_PERSEL_MASK */
#define REF_FLD_WDOG_CFG_PERSEL_SEL13                    0xD0000u	/* vendor WDOG_CFG_PERSEL_SEL13 */
#define REF_FLD_WDOG_CFG_PERSEL_SEL15                    0xF0000u	/* vendor WDOG_CFG_PERSEL_SEL15 */
#define REF_FLD_WDOG_CFG_CLRSRC                          0x1u	/* vendor WDOG_CFG_CLRSRC */
#define REF_FLD_WDOG_CFG_EM2RUN                          0x2u	/* vendor WDOG_CFG_EM2RUN */
#define REF_FLD_WDOG_CFG_EM3RUN                          0x4u	/* vendor WDOG_CFG_EM3RUN */
#define REF_FLD_WDOG_CFG_EM4BLOCK                        0x8u	/* vendor WDOG_CFG_EM4BLOCK */
#define REF_FLD_WDOG_CFG_DEBUGRUN                        0x10u	/* vendor WDOG_CFG_DEBUGRUN */
#define REF_FLD_WDOG_CFG_WDOGRSTDIS                      0x100u	/* vendor WDOG_CFG_WDOGRSTDIS */
#define REF_FLD_WDOG_CMD_CLEAR                           0x1u	/* vendor WDOG_CMD_CLEAR */
#define REF_FLD_WDOG_SYNCBUSY_CMD                        0x1u	/* vendor WDOG_SYNCBUSY_CMD */
#define REF_FLD_WDOG_IF_TOUT                             0x1u	/* vendor WDOG_IF_TOUT */
#define REF_FLD_WDOG_IEN_TOUT                            0x1u	/* vendor WDOG_IEN_TOUT */
#define REF_FLD_WDOG_LOCK_LOCKKEY_LOCK                   0x0u	/* vendor WDOG_LOCK_LOCKKEY_LOCK */
#define REF_FLD_WDOG_LOCK_LOCKKEY_UNLOCK                 0xABE8u	/* vendor WDOG_LOCK_LOCKKEY_UNLOCK */
#define REF_FLD_WDOG_CFG_PERSEL_DEFAULT                  0xF0000u	/* vendor WDOG_CFG_PERSEL_DEFAULT */
#define REF_FLD_USART_EN_EN                              0x1u	/* vendor USART_EN_EN */
#define REF_FLD_USART_CTRL_OVS_SHIFT                     0x5u	/* vendor _USART_CTRL_OVS_SHIFT */
#define REF_FLD_USART_CTRL_OVS_MASK                      0x60u	/* vendor _USART_CTRL_OVS_MASK */
#define REF_FLD_USART_CTRL_OVS_X16                       0x0u	/* vendor USART_CTRL_OVS_X16 */
#define REF_FLD_USART_CTRL_OVS_X8                        0x20u	/* vendor USART_CTRL_OVS_X8 */
#define REF_FLD_USART_FRAME_DATABITS_SHIFT               0x0u	/* vendor _USART_FRAME_DATABITS_SHIFT */
#define REF_FLD_USART_FRAME_DATABITS_MASK                0xFu	/* vendor _USART_FRAME_DATABITS_MASK */
#define REF_FLD_USART_FRAME_DATABITS_EIGHT               0x5u	/* vendor USART_FRAME_DATABITS_EIGHT */
#define REF_FLD_USART_FRAME_PARITY_SHIFT                 0x8u	/* vendor _USART_FRAME_PARITY_SHIFT */
#define REF_FLD_USART_FRAME_PARITY_MASK                  0x300u	/* vendor _USART_FRAME_PARITY_MASK */
#define REF_FLD_USART_FRAME_PARITY_NONE                  0x0u	/* vendor USART_FRAME_PARITY_NONE */
#define REF_FLD_USART_FRAME_STOPBITS_SHIFT               0xCu	/* vendor _USART_FRAME_STOPBITS_SHIFT */
#define REF_FLD_USART_FRAME_STOPBITS_MASK                0x3000u	/* vendor _USART_FRAME_STOPBITS_MASK */
#define REF_FLD_USART_FRAME_STOPBITS_ONE                 0x1000u	/* vendor USART_FRAME_STOPBITS_ONE */
#define REF_FLD_USART_CLKDIV_DIV_MASK                    0x7FFFF8u	/* vendor _USART_CLKDIV_DIV_MASK */
#define REF_FLD_USART_CLKDIV_DIV_SHIFT                   0x3u	/* vendor _USART_CLKDIV_DIV_SHIFT */
#define REF_FLD_USART_CMD_RXEN                           0x1u	/* vendor USART_CMD_RXEN */
#define REF_FLD_USART_CMD_TXEN                           0x4u	/* vendor USART_CMD_TXEN */
#define REF_FLD_USART_CMD_CLEARTX                        0x400u	/* vendor USART_CMD_CLEARTX */
#define REF_FLD_USART_CMD_CLEARRX                        0x800u	/* vendor USART_CMD_CLEARRX */
#define REF_FLD_USART_IF_TXC                             0x1u	/* vendor USART_IF_TXC */
#define REF_FLD_USART_IF_TXBL                            0x2u	/* vendor USART_IF_TXBL */
#define REF_FLD_USART_IF_RXDATAV                         0x4u	/* vendor USART_IF_RXDATAV */
#define REF_FLD_USART_IF_RXOF                            0x10u	/* vendor USART_IF_RXOF */
#define REF_FLD_USART_IF_FERR                            0x200u	/* vendor USART_IF_FERR */
#define REF_FLD_USART_IEN_TXC                            0x1u	/* vendor USART_IEN_TXC */
#define REF_FLD_USART_IEN_TXBL                           0x2u	/* vendor USART_IEN_TXBL */
#define REF_FLD_USART_IEN_RXDATAV                        0x4u	/* vendor USART_IEN_RXDATAV */
#define REF_FLD_USART_IEN_RXOF                           0x10u	/* vendor USART_IEN_RXOF */
#define REF_FLD_USART_IEN_FERR                           0x200u	/* vendor USART_IEN_FERR */
#define REF_FLD_USART_STATUS_RXENS                       0x1u	/* vendor USART_STATUS_RXENS */
#define REF_FLD_USART_STATUS_TXENS                       0x2u	/* vendor USART_STATUS_TXENS */
#define REF_FLD_USART_STATUS_TXC                         0x20u	/* vendor USART_STATUS_TXC */
#define REF_FLD_USART_STATUS_RXDATAV                     0x80u	/* vendor USART_STATUS_RXDATAV */
#define REF_FLD_USART_STATUS_TXIDLE                      0x2000u	/* vendor USART_STATUS_TXIDLE */
#define REF_FLD_USART_RXDATA_RXDATA_MASK                 0xFFu	/* vendor _USART_RXDATA_RXDATA_MASK */
#define REF_FLD_USART_TXDATA_TXDATA_MASK                 0xFFu	/* vendor _USART_TXDATA_TXDATA_MASK */
#define REF_FLD_USART_RXDATAX_RXDATA_MASK                0x1FFu	/* vendor _USART_RXDATAX_RXDATA_MASK */
#define REF_FLD_USART_RXDATAX_PERR                       0x4000u	/* vendor USART_RXDATAX_PERR */
#define REF_FLD_USART_RXDATAX_FERR                       0x8000u	/* vendor USART_RXDATAX_FERR */
#define REF_FLD_SYSCFG_CFGSYSTIC_SYSTICEXTCLKEN          0x1u	/* vendor SYSCFG_CFGSYSTIC_SYSTICEXTCLKEN */
#define REF_FLD_SYSCFG_CFGSYSTIC_SYSTICEXTCLKEN_SHIFT    0x0u	/* vendor _SYSCFG_CFGSYSTIC_SYSTICEXTCLKEN_SHIFT */
#define REF_FLD_SYSCFG_CFGSYSTIC_SYSTICEXTCLKEN_MASK     0x1u	/* vendor _SYSCFG_CFGSYSTIC_SYSTICEXTCLKEN_MASK */
#define REF_FLD_GPIO_USART_ROUTEEN_RXPEN                 0x4u	/* vendor GPIO_USART_ROUTEEN_RXPEN */
#define REF_FLD_GPIO_USART_ROUTEEN_TXPEN                 0x10u	/* vendor GPIO_USART_ROUTEEN_TXPEN */
#define REF_FLD_GPIO_USART_TXROUTE_PORT_SHIFT            0x0u	/* vendor _GPIO_USART_TXROUTE_PORT_SHIFT */
#define REF_FLD_GPIO_USART_TXROUTE_PIN_SHIFT             0x10u	/* vendor _GPIO_USART_TXROUTE_PIN_SHIFT */
#define REF_FLD_GPIO_USART_TXROUTE_MASK                  0xF0003u	/* vendor _GPIO_USART_TXROUTE_MASK */
#define REF_FLD_GPIO_USART_RXROUTE_PORT_SHIFT            0x0u	/* vendor _GPIO_USART_RXROUTE_PORT_SHIFT */
#define REF_FLD_GPIO_USART_RXROUTE_PIN_SHIFT             0x10u	/* vendor _GPIO_USART_RXROUTE_PIN_SHIFT */
#define REF_FLD_GPIO_USART_RXROUTE_MASK                  0xF0003u	/* vendor _GPIO_USART_RXROUTE_MASK */
#define REF_FLD_GPIO_P_MODEL_MODE0_MASK                  0xFu	/* vendor _GPIO_P_MODEL_MODE0_MASK */
#define REF_FLD_GPIO_P_MODEL_MODE0_DISABLED              0x0u	/* vendor GPIO_P_MODEL_MODE0_DISABLED */
#define REF_FLD_GPIO_P_MODEL_MODE0_INPUT                 0x1u	/* vendor GPIO_P_MODEL_MODE0_INPUT */
#define REF_FLD_GPIO_P_MODEL_MODE0_INPUTPULL             0x2u	/* vendor GPIO_P_MODEL_MODE0_INPUTPULL */
#define REF_FLD_GPIO_P_MODEL_MODE0_PUSHPULL              0x4u	/* vendor GPIO_P_MODEL_MODE0_PUSHPULL */
#define REF_FLD_GPIO_P_MODEL_MODE0_WIREDAND              0x8u	/* vendor GPIO_P_MODEL_MODE0_WIREDAND */
#define REF_FLD_GPIO_P_MODEL_MODE0_WIREDANDALTPULLUP     0xEu	/* vendor GPIO_P_MODEL_MODE0_WIREDANDALTPULLUP */

#endif /* MG21_REGS_REF_H */
