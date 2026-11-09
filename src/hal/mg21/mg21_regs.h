/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_regs.h — the EFR32MG21 register map: only what this tree's HAL uses.
 *
 * Transcribed for the ConBee III part (EFR32MG21A010F1024IM32, Cortex-M33 with TrustZone)
 * from the vendor's own description of the part, and in the non-secure views the vendor
 * application uses (docs/hardware_mg21.md §10). The core itself is in the Secure state when this
 * image runs: the bootloader enters it with a plain branch, with no SAU set up and no BLXNS, so
 * the aliases are the vendor application's choice under that same bootloader, not a consequence
 * of the image being non-secure. If a first boot reports bus faults on these addresses, the
 * secure aliases (0x4xxxxxxx, the bootloader's) are the alternative. Two things a SAMR21
 * reader will not expect: the blocks are not dense — the registers sit at the offsets the
 * vendor lists, with gaps that the pad members carry — and enabling a peripheral clock is a
 * property of the peripheral itself (`EN`), not of a clock-enable register: CMU selects each
 * consumer's source and divider.
 *
 * test/unit/test_mg21_regs.c is the guard. Every offset and field value below is checked
 * against the layout of the vendor part header there, because a transcription typo in a base
 * address or a shifted field is invisible until a board misbehaves.
 */

#ifndef MG21_REGS_H
#define MG21_REGS_H

#include <stdint.h>

/* ---- Block bases (non-secure views) ---- */
#define MG21_EMU_BASE 0x50004000u
#define MG21_CMU_BASE 0x50008000u
#define MG21_HFXO0_BASE 0x5000C000u
#define MG21_HFRCO0_BASE 0x50010000u
#define MG21_FSRCO_BASE 0x50018000u
#define MG21_DPLL0_BASE 0x5001C000u
#define MG21_LFRCO_BASE 0x50024000u
#define MG21_MSC_BASE 0x50030000u
#define MG21_PRS_BASE 0x50038000u
#define MG21_GPIO_BASE 0x5003C000u
#define MG21_LDMA_BASE 0x50040000u
#define MG21_USART0_BASE 0x50058000u
#define MG21_WDOG0_BASE 0x5A018000u
#define MG21_SYSCFG_BASE 0x5007C000u

/* ---- Interrupt numbers (the vector order startup_mg21.S lays out) ---- */
#define MG21_IRQ_USART0_RX 11
#define MG21_IRQ_USART0_TX 12
#define MG21_IRQ_WDOG0 43
#define MG21_IRQ_SYSTICK (-1)
#define MG21_IRQ_PENDSV (-2)
#define MG21_IRQ_SVCALL (-5)
#define MG21_IRQ_HARDFAULT (-13)

/* Priority field width: MG21's NVIC takes four bits per priority, not the SAMR21's three. */
#define MG21_NVIC_PRIO_BITS 4u
#define MG21_NVIC_PRIO_MAX ((1u << MG21_NVIC_PRIO_BITS) - 1u)

/* ---- Part geometry (docs/hardware_mg21.md §10.2, docs/hardware_mg21.md §10.7) ---- */
#define MG21_FLASH_SIZE 0x100000u
#define MG21_FLASH_PAGE_SIZE 0x2000u
#define MG21_USERDATA_BASE 0x0FE00000u
#define MG21_USERDATA_SIZE 0x400u

/* ---- Nominal oscillator rates (datasheet tables, not registers) ----
 * FSRCO, LFRCO and ULFRCO are the part's; the 38.4 MHz is the ConBee III's crystal (HFXO is trimmed
 * from the manufacturing token, docs/hardware_mg21.md §10.1). The two RC oscillators' tolerance is
 * +-3 % and +-10 % (EFR32MG21 datasheet: LFRCO 31.785/32.768/33.751 kHz, ULFRCO 0.944/1/1.095 kHz),
 * which is the honesty limit of every millisecond this board measures. */
#define MG21_FSRCO_HZ 20000000u
#define MG21_LFRCO_HZ 32768u
#define MG21_ULFRCO_HZ 1000u
#define MG21_BOARD_HFXO_HZ 38400000u

/* ---- CMU (base MG21_CMU_BASE) ----
 * The clock hub: SYSCLKCTRL picks the system clock and the two bus prescalers, the group
 * registers pick a consumer's source, and WDOGLOCK gates the watchdog's own clock select. */
typedef struct
{
    volatile uint32_t IPVERSION;   /* +0x00 */
    uint32_t pad0;                 /* +0x04 */
    volatile uint32_t STATUS;      /* +0x08 */
    uint32_t pad1;                 /* +0x0C */
    volatile uint32_t LOCK;        /* +0x10 */
    volatile uint32_t WDOGLOCK;    /* +0x14 */
    uint32_t pad2[2];              /* +0x18 */
    volatile uint32_t IF;          /* +0x20 */
    volatile uint32_t IEN;         /* +0x24 */
    uint32_t pad3[10];             /* +0x28 */
    volatile uint32_t CALCMD;      /* +0x50 */
    volatile uint32_t CALCTRL;     /* +0x54 */
    volatile uint32_t CALCNT;      /* +0x58 */
    uint32_t pad4[5];              /* +0x5C */
    volatile uint32_t SYSCLKCTRL;  /* +0x70 */
    uint32_t pad5[43];             /* +0x74 */
    volatile uint32_t EM01GRPACLKCTRL; /* +0x120 */
    uint32_t pad6[7];              /* +0x124 */
    volatile uint32_t EM23GRPACLKCTRL; /* +0x140 */
    uint32_t pad7[47];             /* +0x144 */
    volatile uint32_t WDOG0CLKCTRL; /* +0x200 */
    uint32_t pad8[31];             /* +0x204 */
    volatile uint32_t RADIOCLKCTRL; /* +0x280 */
} Mg21Cmu;   /* the block goes on; the struct stops where this tree's use stops */

/* SYSCLKCTRL: CLKSEL [2:0], HCLKPRESC [13:12], PCLKPRESC [10]. The reset selection is FSRCO
 * with both prescalers at divide-by-one, so code runs from the 20 MHz fast-start RC until it
 * says otherwise, and every timing constant has to be computed after the switch. */
#define MG21_CMU_SYSCLKCTRL_CLKSEL_SHIFT 0u
#define MG21_CMU_SYSCLKCTRL_CLKSEL_MASK 0x7u
#define MG21_CMU_SYSCLKCTRL_CLKSEL_FSRCO 0x1u    /* fixed 20 MHz, the reset source */
#define MG21_CMU_SYSCLKCTRL_CLKSEL_HFRCODPLL 0x2u
#define MG21_CMU_SYSCLKCTRL_CLKSEL_HFXO 0x3u
#define MG21_CMU_SYSCLKCTRL_HCLKPRESC_SHIFT 12u
#define MG21_CMU_SYSCLKCTRL_HCLKPRESC_MASK 0x3000u
#define MG21_CMU_SYSCLKCTRL_PCLKPRESC_SHIFT 10u
#define MG21_CMU_SYSCLKCTRL_PCLKPRESC_MASK 0x400u
/* The field encoding is divide-by-(value+1): a zero field means no divide. */
#define MG21_CMU_PRESC_DIV(value) ((value) + 1u)

/* EM01GRPACLKCTRL: the source for the EM0/EM1 group A peripherals — USART0, GPIO, PRS, LDMA, I2C1
 * (docs/hardware_mg21.md §10.3) — same field shape. Its *reset* selection is HFRCODPLL, an
 * oscillator that is not running at reset, so a driver that never writes this register clocks its
 * USART from nothing. */
#define MG21_CMU_EM01GRPACLKCTRL_CLKSEL_SHIFT 0u
#define MG21_CMU_EM01GRPACLKCTRL_CLKSEL_MASK 0x7u
#define MG21_CMU_EM01GRPACLKCTRL_CLKSEL_HFRCODPLL 0x1u
#define MG21_CMU_EM01GRPACLKCTRL_CLKSEL_HFXO 0x2u
#define MG21_CMU_EM01GRPACLKCTRL_CLKSEL_FSRCO 0x4u

/* EM23GRPACLKCTRL: the low-energy group A clock — LETIMER, RTCC and the core SysTick's alternate
 * clock (docs/hardware_mg21.md §10.3, reference manual figure 8.1) — three sources, two field bits,
 * reset value LFRCO. */
#define MG21_CMU_EM23GRPACLKCTRL_CLKSEL_SHIFT 0u
#define MG21_CMU_EM23GRPACLKCTRL_CLKSEL_MASK 0x3u
#define MG21_CMU_EM23GRPACLKCTRL_CLKSEL_LFRCO 0x1u
#define MG21_CMU_EM23GRPACLKCTRL_CLKSEL_LFXO 0x2u
#define MG21_CMU_EM23GRPACLKCTRL_CLKSEL_ULFRCO 0x3u

/* WDOG0CLKCTRL: the watchdog's clock, reset to LFRCO. */
#define MG21_CMU_WDOG0CLKCTRL_CLKSEL_SHIFT 0u
#define MG21_CMU_WDOG0CLKCTRL_CLKSEL_MASK 0x7u
#define MG21_CMU_WDOG0CLKCTRL_CLKSEL_LFRCO 0x1u
#define MG21_CMU_WDOG0CLKCTRL_CLKSEL_ULFRCO 0x3u

/* RADIOCLKCTRL: one bit, the radio subsystem's clock. It resets to 0, and while it is 0 every access
 * to a radio block or to SEQRAM/FRCRAM bus-faults - measured on the bench with the register monitor
 * (docs/hardware_mg21.md §10.5); the vendor application writes it to 1 in its init. */
#define MG21_CMU_RADIOCLKCTRL_EN 0x1u

/* CMU_LOCK / CMU_WDOGLOCK: write the key to open the protected writes. Both use the same
 * key value; WDOGLOCK gates the watchdog clock select only. */
#define MG21_CMU_LOCKKEY_UNLOCK 0x93F7u

#define MG21_CMU_STATUS_CALRDY 0x1u
#define MG21_CMU_CALCMD_CALSTART 0x1u
#define MG21_CMU_CALCTRL_CONT 0x800000u

/* ---- HFXO0 (base MG21_HFXO0_BASE) ----
 * The 38.4 MHz crystal the ConBee III board carries. It has no enable bit of its own in the
 * sense a SAMR21 reader expects: CTRL selects on-demand versus forced, and STATUS reports
 * both the enable state and the ready state. */
typedef struct
{
    uint32_t pad0[4];           /* +0x00 */
    volatile uint32_t XTALCFG;  /* +0x10 */
    uint32_t pad1[1];           /* +0x14 */
    volatile uint32_t XTALCTRL; /* +0x18 */
    uint32_t pad2[1];           /* +0x1C */
    volatile uint32_t CFG;      /* +0x20 */
    uint32_t pad3[1];           /* +0x24 */
    volatile uint32_t CTRL;     /* +0x28 */
    uint32_t pad4[9];           /* +0x2C */
    volatile uint32_t CMD;      /* +0x50 */
    uint32_t pad5[1];           /* +0x54 */
    volatile uint32_t STATUS;   /* +0x58 */
    uint32_t pad6[5];           /* +0x5C */
    volatile uint32_t IF;       /* +0x70 */
    volatile uint32_t IEN;      /* +0x74 */
    uint32_t pad7[2];           /* +0x78 */
    volatile uint32_t LOCK;     /* +0x80 */
} Mg21Hfxo;

#define MG21_HFXO_CTRL_FORCEEN 0x1u      /* hold the oscillator on regardless of requests */
#define MG21_HFXO_CTRL_DISONDEMAND 0x2u  /* do not turn it off when the last request goes away */
#define MG21_HFXO_CTRL_FORCEXI2GNDANA 0x10u
#define MG21_HFXO_CTRL_FORCEXO2GNDANA 0x20u
#define MG21_HFXO_STATUS_RDY 0x1u
#define MG21_HFXO_STATUS_ENS 0x10000u    /* enabled state */
#define MG21_HFXO_STATUS_HWREQ 0x20000u  /* a hardware consumer is asking for it */
#define MG21_HFXO_CMD_COREBIASOPT 0x1u
#define MG21_HFXO_IF_RDY 0x1u
#define MG21_HFXO_CFG_MODE 0x1u          /* crystal mode (as opposed to an external clock in) */
/* HFXO's own configuration lock wants a third key: CMU 0x93F7, WDOG0 0xABE8, HFXO0 0x580E. */
#define MG21_HFXO_LOCK_LOCKKEY_UNLOCK 0x580Eu

/* ---- LFRCO (base MG21_LFRCO_BASE) ----
 * The 32.768 kHz RC oscillator: the watchdog's clock, and the only timer that keeps running
 * in EM2/EM3. Like HFXO it reports enable and ready rather than taking an enable command. */
typedef struct
{
    uint32_t pad0[2];          /* +0x00 */
    volatile uint32_t STATUS;  /* +0x08 */
    volatile uint32_t CAL;     /* +0x0C trim value */
    uint32_t pad1[1];          /* +0x10 */
    volatile uint32_t IF;      /* +0x14 */
    volatile uint32_t IEN;     /* +0x18 */
    volatile uint32_t SYNCBUSY; /* +0x1C */
    volatile uint32_t LOCK;    /* +0x20 */
} Mg21Lfrco;

#define MG21_LFRCO_STATUS_RDY 0x1u
#define MG21_LFRCO_STATUS_ENS 0x10000u
#define MG21_LFRCO_CAL_MASK 0xFFu
#define MG21_LFRCO_SYNCBUSY_MASK 0x1u

/* ---- WDOG0 (base MG21_WDOG0_BASE) ----
 * The reset watchdog. Its timeout is one of sixteen selections in CFG, counted in cycles of
 * whatever CMU_WDOG0CLKCTRL selected, and a write to CMD clears the counter. Changing the
 * clock source goes through CMU->WDOGLOCK, not through this block. */
typedef struct
{
    volatile uint32_t IPVERSION; /* +0x00 */
    volatile uint32_t EN;        /* +0x04 */
    volatile uint32_t CFG;       /* +0x08 */
    volatile uint32_t CMD;       /* +0x0C */
    uint32_t pad0;               /* +0x10 */
    volatile uint32_t STATUS;    /* +0x14 */
    volatile uint32_t IF;        /* +0x18 */
    volatile uint32_t IEN;       /* +0x1C */
    volatile uint32_t LOCK;      /* +0x20 */
    volatile uint32_t SYNCBUSY;  /* +0x24 */
} Mg21Wdog;

#define MG21_WDOG_EN_EN 0x1u
#define MG21_WDOG_CFG_CLRSRC 0x1u      /* 0 = a write to CMD clears; 1 = PRS source 0 clears */
#define MG21_WDOG_CFG_EM2RUN 0x2u
#define MG21_WDOG_CFG_EM3RUN 0x4u
#define MG21_WDOG_CFG_EM4BLOCK 0x8u
#define MG21_WDOG_CFG_DEBUGRUN 0x10u
#define MG21_WDOG_CFG_WDOGRSTDIS 0x100u /* 0 = timeout resets, 1 = timeout only interrupts */
#define MG21_WDOG_CFG_PERSEL_SHIFT 16u
#define MG21_WDOG_CFG_PERSEL_MASK 0xF0000u
#define MG21_WDOG_CMD_CLEAR 0x1u
#define MG21_WDOG_SYNCBUSY_CMD 0x1u
#define MG21_WDOG_IF_TOUT 0x1u
#define MG21_WDOG_IEN_TOUT 0x1u
/* The watchdog's own configuration lock. Its key is *not* the CMU key: WDOG0->LOCK wants 0xABE8,
 * CMU->LOCK and CMU->WDOGLOCK want 0x93F7. */
#define MG21_WDOG_LOCK_LOCKKEY_LOCK 0x0u
#define MG21_WDOG_LOCK_LOCKKEY_UNLOCK 0xABE8u

/* The timeout each PERSEL selection counts, from the vendor's own period enum
 * (docs/hardware_mg21.md §10.3): selection n counts 2^(n+3) + 1 cycles. */
#define MG21_WDOG_PERSEL_MIN 0u
#define MG21_WDOG_PERSEL_MAX 15u
#define MG21_WDOG_PERIOD_CYCLES(sel) (((1u << ((sel) + 3u)) + 1u))
#define MG21_WDOG_PERSEL_DEFAULT 15u /* the register's reset selection */
#define MG21_WDOG_PERSEL_2S 13u      /* 65537 LFRCO cycles = 2.000 s at 32.768 kHz */
#define MG21_WDOG_PERSEL_8S 15u      /* 262145 LFRCO cycles = 8.000 s at 32.768 kHz: what the
                                       * shipped bootloader armed (§10.5, the deSiLabs
                                       * bootloader's de_BTL_wdt.c) */
#define MG21_WDOG_PERSEL_2S_ULFRCO 8u /* 2049 ULFRCO cycles = 2.049 s at 1 kHz: the fallback when
                                        * LFRCO does not come up */

/* ---- USART0 (base MG21_USART0_BASE) ----
 * The wired host link on this board. EN turns the block on, CMD enables the transmitter and
 * the receiver separately, CLKDIV carries the divisor with five fractional bits, and the frame
 * format lives in FRAME. */
typedef struct
{
    uint32_t pad0;              /* +0x00 */
    volatile uint32_t EN;       /* +0x04 */
    volatile uint32_t CTRL;     /* +0x08 */
    volatile uint32_t FRAME;    /* +0x0C */
    volatile uint32_t TRIGCTRL; /* +0x10 */
    volatile uint32_t CMD;      /* +0x14 */
    volatile uint32_t STATUS;   /* +0x18 */
    volatile uint32_t CLKDIV;   /* +0x1C */
    volatile uint32_t RXDATAX;  /* +0x20 */
    volatile uint32_t RXDATA;   /* +0x24 */
    uint32_t pad1[4];           /* +0x28 */
    volatile uint32_t TXDATAX;  /* +0x38 */
    volatile uint32_t TXDATA;   /* +0x3C */
    uint32_t pad2[2];           /* +0x40 */
    volatile uint32_t IF;       /* +0x48 */
    volatile uint32_t IEN;      /* +0x4C */
} Mg21Usart;

/* The interrupt flags are cleared through the block's CLR alias, never through IF itself: on this
 * part a direct write to an interrupt flag register "will have no effect" (reference manual 3.2,
 * 4.2.4.4.6). TXBL and RXDATAV follow their condition in hardware; TXC, RXOF and FERR are sticky and
 * stay set until written here. */
#define MG21_USART_IF_CLR_OFFSET 0x2048u
#define MG21_USART0_IF_CLR (*(volatile uint32_t *)(MG21_USART0_BASE + MG21_USART_IF_CLR_OFFSET))

#define MG21_USART_EN_EN 0x1u
#define MG21_USART_CTRL_OVS_SHIFT 5u
#define MG21_USART_CTRL_OVS_MASK 0x60u
#define MG21_USART_CTRL_OVS_X16 0x0u
#define MG21_USART_CTRL_OVS_X8 0x20u
#define MG21_USART_FRAME_DATABITS_SHIFT 0u
#define MG21_USART_FRAME_DATABITS_MASK 0xFu
#define MG21_USART_FRAME_DATABITS_EIGHT 0x5u
#define MG21_USART_FRAME_PARITY_SHIFT 8u
#define MG21_USART_FRAME_PARITY_MASK 0x300u
#define MG21_USART_FRAME_PARITY_NONE 0x0u
#define MG21_USART_FRAME_STOPBITS_SHIFT 12u
#define MG21_USART_FRAME_STOPBITS_MASK 0x3000u
#define MG21_USART_FRAME_STOPBITS_ONE 0x1000u
#define MG21_USART_CLKDIV_MASK 0x7FFFF8u /* the divisor times 256, five fractional bits */
#define MG21_USART_CMD_RXEN 0x1u
#define MG21_USART_CMD_TXEN 0x4u
#define MG21_USART_CMD_CLEARTX 0x400u
#define MG21_USART_CMD_CLEARRX 0x800u
#define MG21_USART_IF_TXC 0x1u      /* transmission complete */
#define MG21_USART_IF_TXBL 0x2u     /* transmit buffer low */
#define MG21_USART_IF_RXDATAV 0x4u  /* receive data valid */
#define MG21_USART_IF_RXOF 0x10u    /* receive overflow */
#define MG21_USART_IF_FERR 0x200u   /* framing error */
#define MG21_USART_IEN_TXC 0x1u
#define MG21_USART_IEN_TXBL 0x2u
#define MG21_USART_IEN_RXDATAV 0x4u
#define MG21_USART_IEN_RXOF 0x10u
#define MG21_USART_IEN_FERR 0x200u
#define MG21_USART_STATUS_RXENS 0x1u
#define MG21_USART_STATUS_TXENS 0x2u
#define MG21_USART_STATUS_TXC 0x20u
#define MG21_USART_STATUS_RXDATAV 0x80u
#define MG21_USART_STATUS_TXIDLE 0x2000u
#define MG21_USART_DATA_MASK 0xFFu
#define MG21_USART_RXDATAX_DATA_MASK 0x1FFu
/* What the receiver says about the octet in RXDATA travels beside it, in RXDATAX: framing and
 * parity errors are flags on the *byte*, not a separate status register as on the SERCOM. */
#define MG21_USART_RXDATAX_PERR 0x4000u
#define MG21_USART_RXDATAX_FERR 0x8000u

/* ---- GPIO (base MG21_GPIO_BASE) ----
 * Pin state per port, and the peripheral pad routing in a separate array: on this part a
 * USART's pads are chosen by the GPIO block, not by the USART (contrast the SAMR21, where
 * the SERCOM names its own pads). */
typedef struct
{
    volatile uint32_t CTRL;   /* +0x00 */
    volatile uint32_t MODEL;  /* +0x04 pins 0..7, four bits each */
    uint32_t pad0;            /* +0x08 */
    volatile uint32_t MODEH;  /* +0x0C pins 8..15, four bits each */
    volatile uint32_t DOUT;   /* +0x10 */
    volatile uint32_t DIN;    /* +0x14 */
    uint32_t pad1[6];         /* +0x18: the port block is 48 bytes wide */
} Mg21GpioPort;

typedef struct
{
    volatile uint32_t ROUTEEN;   /* +0x00 which pads this peripheral drives */
    volatile uint32_t CSROUTE;   /* +0x04 */
    volatile uint32_t CTSROUTE;  /* +0x08 */
    volatile uint32_t RTSROUTE;  /* +0x0C */
    volatile uint32_t RXROUTE;   /* +0x10 */
    volatile uint32_t CLKROUTE;  /* +0x14 */
    volatile uint32_t TXROUTE;   /* +0x18 */
    uint32_t pad0;               /* +0x1C */
} Mg21UsartRoute;

typedef struct
{
    Mg21GpioPort P[2];      /* +0x00 port A (PA0..PA15), +0x30 port B (PB0..PB15) */
    uint32_t pad0[168];    /* +0x60 */
    volatile uint32_t LOCK; /* +0x300 */
    uint32_t pad1[170];    /* +0x304 */
    Mg21UsartRoute USARTROUTE[4]; /* +0x5AC, 32 bytes each */
} Mg21Gpio;

/* Port geometry as the part lays it out. */
#define MG21_GPIO_PORT_STRIDE 48u
#define MG21_GPIO_USARTROUTE_STRIDE 32u

/* A pin mode is four bits in MODEL (pins 0..7) or MODEH (pins 8..15). */
/* One pin in the port's DOUT / DIN / MODE words: the bit for pins 0..15, and the four-bit field for
 * the same pin in MODEL (0..7) or MODEH (8..15). */
#define MG21_GPIO_PIN_BIT(pin) (1u << (pin))
#define MG21_GPIO_MODE_SHIFT(pin) (4u * ((pin) & 7u))
#define MG21_GPIO_MODE_MASK(pin) (0xFu << MG21_GPIO_MODE_SHIFT(pin))
#define MG21_GPIO_MODE_HIGH(pin) (((pin) & 8u) != 0u)
#define MG21_GPIO_MODE_DISABLED 0x0u
#define MG21_GPIO_MODE_INPUT 0x1u
#define MG21_GPIO_MODE_INPUTPULL 0x2u
#define MG21_GPIO_MODE_PUSHPULL 0x4u
#define MG21_GPIO_MODE_WIREDAND 0x8u
#define MG21_GPIO_MODE_WIREDANDALTPULLUP 0xEu

/* USART pad routing: a route word carries the port number in [3:0] and the pin number in
 * [19:16]; ROUTEEN's bits switch the pad from GPIO to the peripheral. */
#define MG21_GPIO_USART_ROUTEEN_RXPEN 0x4u
#define MG21_GPIO_USART_ROUTEEN_TXPEN 0x10u
#define MG21_GPIO_USARTROUTE_PORT_SHIFT 0u
#define MG21_GPIO_USARTROUTE_PIN_SHIFT 16u
#define MG21_GPIO_USARTROUTE_MASK 0xF0003u
#define MG21_GPIO_USARTROUTE(port, pin)                                                                \
    ((((port) & 0x3u) << MG21_GPIO_USARTROUTE_PORT_SHIFT)                                              \
     | (((pin) & 0xFu) << MG21_GPIO_USARTROUTE_PIN_SHIFT))

/* ---- Radio subsystem: what a hand-back before a reset needs ----
 * The radio blocks are the radio driver's (docs/hardware_mg21.md §10.4); this is only the subset an
 * image needs to switch them off again before a reset (docs/hardware_mg21.md §10.5).
 * Checked against test/data/mg21_radio_ref.h by the mg21_regs suite. Every block has EN at +0x04,
 * bit 0; BUFC sits in the peripheral window, the others in the radio window. */
#define MG21_RAC_BASE 0xB8020000u
#define MG21_FRC_BASE 0xB8004000u
#define MG21_MODEM_BASE 0xB8014000u
#define MG21_SYNTH_BASE 0xB8018000u
#define MG21_AGC_BASE 0xB800C000u
#define MG21_PROTIMER_BASE 0xB801C000u
#define MG21_RFCRC_BASE 0xB8010000u
#define MG21_BUFC_BASE 0x54004000u

#define MG21_RADIO_EN_OFFSET 0x04u
#define MG21_RADIO_EN_EN 0x1u
#define MG21_RADIO_SET_ALIAS 0x1000u /* the block's SET window, as for every Series 2 block */

#define MG21_RAC_RXENSRCEN_OFFSET 0x08u
#define MG21_RAC_STATUS_OFFSET 0x0Cu
#define MG21_RAC_CTRL_OFFSET 0x14u
#define MG21_RAC_SEQCMD_OFFSET 0x2Cu
#define MG21_RAC_VECTADDR_OFFSET 0x64u
#define MG21_RAC_SEQCTRL_OFFSET 0x68u
#define MG21_RAC_CTRL_FORCEDISABLE 0x1u
#define MG21_RAC_STATUS_STATE_MASK 0x0F000000u
#define MG21_RAC_STATUS_STATE_SHIFT 24u
#define MG21_RAC_STATUS_STATE_OFF 0u

#define MG21_RADIO_REG(base, off) (*(volatile uint32_t *)((base) + (off)))

/* ---- Radio subsystem: what the receive back-end drives (hal/mg21/own/mg21_radio.c) ----
 * Offsets and fields the bring-up, the channel tune and the receive path use, checked against
 * test/data/mg21_radio_ref.h by the mg21_regs suite. The values written are the generated tables'
 * (mg21_radio_cfg.h, docs/hardware_mg21.md §10.5). */
#define MG21_RADIO_CLR_ALIAS 0x2000u /* the block's CLR window */

#define MG21_RAC_CMD_OFFSET 0x10u
#define MG21_RAC_R6_OFFSET 0x4Cu
#define MG21_RAC_SR0_OFFSET 0x70u
#define MG21_RAC_SR1_OFFSET 0x74u
#define MG21_RAC_SR2_OFFSET 0x78u
#define MG21_RAC_DIGCLKRETIMECTRL_OFFSET 0x15Cu
#define MG21_RAC_XORETIMECTRL_OFFSET 0x164u
#define MG21_RAC_CMD_TXEN 0x1u
#define MG21_RAC_CMD_CLEARRXOVERFLOW 0x40u
#define MG21_RAC_CMD_RXCAL 0x80u
#define MG21_RAC_RXENSRCEN_SWRXEN 0x1u
#define MG21_RAC_SEQCTRL_COMPACT 0x1u /* the sequencer timer continues past its compare */
#define MG21_RAC_STATUS_STATE_RXSEARCH 2u
#define MG21_RAC_STATUS_STATE_RXOVERFLOW 6u

#define MG21_FRC_WCNTCMP2_OFFSET 0x28u
#define MG21_FRC_CMD_OFFSET 0x2Cu
#define MG21_FRC_CMD_CONVINIT 0x200u
#define MG21_FRC_IF_OFFSET 0x68u
#define MG21_FRC_IF_TXDONE 0x1u

#define MG21_MODEM_SRCCHF_OFFSET 0xECu
#define MG21_MODEM_DIGMIXCTRL_OFFSET 0x10Cu
#define MG21_MODEM_RAMPCTRL_OFFSET 0xC0u
#define MG21_MODEM_RAMPLEV_OFFSET 0xC4u
#define MG21_MODEM_RAMPCTRL_RATES_MASK 0xFFFu /* RAMPRATE0..2, four bits each */
#define MG21_MODEM_IRCALCOEFWR0_OFFSET 0x250u
#define MG21_MODEM_IRCALCOEFWR1_OFFSET 0x254u
#define MG21_MODEM_DIGMIXCTRL_DIGMIXFREQ_MASK 0x000FFFFFu
/* The SRCCHF fields the channel tune rewrites: all but the top nibble and bits 10:8. */
#define MG21_MODEM_SRCCHF_TUNED_MASK 0x0FFFF8FFu

#define MG21_SYNTH_FREQ_OFFSET 0x34u
#define MG21_SYNTH_CHCTRL_OFFSET 0x40u
#define MG21_SYNTH_VCOTUNING_OFFSET 0x4Cu

#define MG21_AGC_RSSI_OFFSET 0x10u

/* BUFC: four buffers of the same layout, 0x30 apart. A buffer's size is 64 << CTRL.SIZE bytes, and
 * its ADDR holds the low 24 bits of the buffer's address above RAMBASEADDR (0x20000000 at reset). */
#define MG21_SRAM_BASE 0x20000000u /* BUFC.RAMBASEADDR's reset value: the buffers' window starts here */
#define MG21_BUFC_BUF_STRIDE 0x30u
#define MG21_BUFC_BUF0_CTRL_OFFSET 0x08u
#define MG21_BUFC_BUF0_ADDR_OFFSET 0x0Cu
#define MG21_BUFC_BUF0_READOFFSET_OFFSET 0x14u /* bytes read so far; the buffer index is it mod size */
#define MG21_BUFC_BUF0_READDATA_OFFSET 0x1Cu
#define MG21_BUFC_BUF0_WRITEDATA_OFFSET 0x20u
#define MG21_BUFC_BUF0_STATUS_OFFSET 0x28u
#define MG21_BUFC_BUF0_CMD_OFFSET 0x30u
#define MG21_BUFC_BUF_STATUS_BYTES_MASK 0x1FFFu
#define MG21_BUFC_BUF_CMD_CLEAR 0x1u

/* SEQRAM, where the sequencer runs from, as the CPU and the sequencer both address it here; and the
 * per-part IR calibration pair in DEVINFO (docs/hardware_mg21.md §10.5). */
#define MG21_SEQRAM_BASE 0xB0000000u
#define MG21_SEQRAM_BYTES 0x2000u
#define MG21_SEQRAM_STACK 0xB0001F88u /* R6, the sequencer's call stack */
#define MG21_DEVINFO_IRCAL0 0x0FE08248u
#define MG21_DEVINFO_IRCAL1 0x0FE0824Cu

/* ---- ARMv8-M core, as the SAMR21 side sees ARMv6-M ----
 * Same registers, wider priorities, and the AIRCR key the core insists on. */
#define MG21_SCB_VTOR 0xE000ED08u
#define MG21_SCB_AIRCR 0xE000ED0Cu
/* The key ARM requires in AIRCR[31:16] before the core accepts a write there; 0x5FA is the value,
 * 16 is where it goes, and this tree holds them apart so a change to one is visible. */
#define MG21_AIRCR_VECTKEY_VALUE 0x5FAu
#define MG21_AIRCR_VECTKEY_POS 16u
#define MG21_AIRCR_VECTKEY (MG21_AIRCR_VECTKEY_VALUE << MG21_AIRCR_VECTKEY_POS)
#define MG21_AIRCR_SYSRESETREQ (1u << 2)
#define MG21_NVIC_ISER0 0xE000E100u
#define MG21_NVIC_ICER0 0xE000E180u
#define MG21_NVIC_ICER1 0xE000E184u
#define MG21_NVIC_ICPR0 0xE000E280u
#define MG21_NVIC_ICPR1 0xE000E284u
#define MG21_NVIC_IPRI_B0 0xE000E400u /* four priority bits, one byte per interrupt */
/* Four priority bits, one byte per interrupt, and the left shift that puts a priority value in the
 * implemented top half of that byte (the ARMv8-M convention CMSIS uses). */
#define MG21_NVIC_PRIO_SHIFT (8u - MG21_NVIC_PRIO_BITS)
#define MG21_NVIC_PRIO(value) (((value) << MG21_NVIC_PRIO_SHIFT) & 0xFFu)
#define MG21_NVIC_GROUP(irq) (((uint32_t)(irq)) >> 5u)
#define MG21_NVIC_BIT(irq) (1uL << (((uint32_t)(irq)) & 31u))
#define MG21_SYSTICK_CTRL 0xE000E010u
#define MG21_SYSTICK_LOAD 0xE000E014u
#define MG21_SYSTICK_VAL 0xE000E018u
#define MG21_SYSTICK_CALIB 0xE000E01Cu
#define MG21_SYSTICK_CTRL_ENABLE (1u << 0)
#define MG21_SYSTICK_CTRL_TICKINT (1u << 1)
#define MG21_SYSTICK_CTRL_CLKSOURCE (1u << 2) /* 1 = the processor clock, not an external one */
#define MG21_SYSTICK_CTRL_COUNTFLAG (1u << 16)
#define MG21_SYSTICK_RELOAD_MAX 0x00FFFFFFu
#define MG21_SYSTICK_PRIORITY 0u

/* SYSCFG (base MG21_SYSCFG_BASE): the SysTick alternate clock is gated here, not in the core. The
 * core's CLKSOURCE bit then chooses between that clock and the processor clock. */
typedef struct
{
    uint32_t pad0[4];             /* +0x00: IF, IEN, and the gap to the revision words */
    volatile uint32_t CHIPREVHW;  /* +0x10 hardwired chip revision */
    volatile uint32_t CHIPREV;    /* +0x14 part family and revision */
    uint32_t pad1[3];             /* +0x18 */
    volatile uint32_t CFGSYSTIC;  /* +0x24 SysTick clock source */
} Mg21Syscfg;

#define MG21_SYSCFG_CFGSYSTIC_SYSTICEXTCLKEN 0x1u

/* ---- TrustZone attribution: SMU's peripheral protection and the core's SAU ----
 * Out of reset every peripheral is secure in SMU's PPU, the SAU is off with ALLNS clear, and the
 * core runs Secure, so the CPU's accesses are secure transactions whichever alias they name. The
 * radio's sequencer reaches EMU and BUFC through their non-secure aliases with non-secure
 * transactions, and the PPU rejects those while the peripheral is secure (docs/hardware_mg21.md
 * §10.19). SMU is reached only through its secure alias, and stays secure. Checked against
 * test/data/mg21_regs_ref.h by the mg21_regs suite. */
#define MG21_SMU_S_BASE 0x44008000u
#define MG21_SMU_PPUSATD0_OFFSET 0x60u
#define MG21_SMU_PPUSATD1_OFFSET 0x64u
#define MG21_SMU_SET_ALIAS 0x1000u /* SMU's SET and CLR windows, as for every Series 2 block */
#define MG21_SMU_CLR_ALIAS 0x2000u
#define MG21_SMU_PPUSATD0_MASK 0xFFFFFFFEu /* one bit per peripheral, 1 = secure */
#define MG21_SMU_PPUSATD1_MASK 0x00003FFFu
#define MG21_SMU_PPUSATD1_SMU (1u << 11)
#define MG21_SMU_PPUSATD1_SEMAILBOX (1u << 13)
/* The SAU is the architecture's (ARMv8-M: SAU_CTRL at 0xE000EDD0, ALLNS bit 1), not the part's, and
 * CMSIS declares it only for a secure-state compile, so it has no entry in the record. With the SAU
 * off and ALLNS set the IDAU alone attributes: the aliases with bit 28 set are non-secure. */
#define MG21_SAU_CTRL 0xE000EDD0u
#define MG21_SAU_CTRL_ALLNS (1u << 1)

/* ---- Peripheral views ----
 * The blocks are structs with volatile members; one cast per block, at the base this tree's
 * transcription names. A driver reaching for a peripheral reaches for one of these. */
#define MG21_CMU ((Mg21Cmu *)MG21_CMU_BASE)
#define MG21_HFXO0 ((Mg21Hfxo *)MG21_HFXO0_BASE)
#define MG21_LFRCO ((Mg21Lfrco *)MG21_LFRCO_BASE)
#define MG21_WDOG0 ((Mg21Wdog *)MG21_WDOG0_BASE)
#define MG21_USART0 ((Mg21Usart *)MG21_USART0_BASE)
#define MG21_GPIO ((Mg21Gpio *)MG21_GPIO_BASE)
#define MG21_SYSCFG ((Mg21Syscfg *)MG21_SYSCFG_BASE)
#define MG21_GPIO_PORT(n) (&MG21_GPIO->P[n])
#define MG21_USART_ROUTE(n) (&MG21_GPIO->USARTROUTE[n])

/* Fault enabling and the cycle counter: ARMv8-M makes the memory, bus and usage faults separate
 * vectors that are off until enabled, and DWT's CYCCNT is the only cycle-level clock a
 * non-secure image can read without the radio. */
#define MG21_SCB_SHCSR 0xE000ED24u
#define MG21_SCB_SHCSR_MEMFAULTENA 0x10000u
#define MG21_SCB_SHCSR_BUSFAULTENA 0x20000u
#define MG21_SCB_SHCSR_USGFAULTENA 0x40000u
#define MG21_SCB_DEMCR 0xE000EDFCu
#define MG21_DEMCR_TRCENA (1u << 24)
#define MG21_DWT_CTRL 0xE0001000u
#define MG21_DWT_CYCCNT 0xE0001004u
#define MG21_DWT_CTRL_CYCCNTENA 0x1u

#endif /* MG21_REGS_H */
