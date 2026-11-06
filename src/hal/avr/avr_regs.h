/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_regs.h — this tree's register map of the ATmega256RFR2 (docs/hardware_avr.md §11).
 *
 * Written from the datasheet's register summary, not from avr-libc: the image links no avr-libc and
 * includes none of its headers. The transcription is checked by the avr_regs suite against avr-libc's
 * iom256rfr2.h (docs/testing.md §2), and the check is by name: every AVR_<name> here is an address or
 * a bit number that avr-libc spells <name>, and the suite compares each one with avr-libc's, so a
 * name added here is checked without being listed. The few names that are this tree's own carry the
 * AVR_OWN_ prefix and are not compared.
 *
 * Addresses are data-space addresses, as the C side reads them with a load or a store. An I/O
 * register below 0x60 is also reachable with in/out at its address minus 0x20 (AVR_IO), which is the
 * form the startup uses. Bit fields are given as bit numbers, as the datasheet names them; AVR_BIT
 * makes a mask. Every value is written without a suffix so that startup_avr.S can include the same
 * file. */
#ifndef AVR_REGS_H
#define AVR_REGS_H

/* ---- Memories ---- */
#define AVR_RAMSTART 0x200
#define AVR_RAMEND 0x81FF
#define AVR_E2END 0x1FFF
#define AVR_FLASHEND 0x3FFFF
#define AVR_SPM_PAGESIZE 256

/* ---- Core ---- */
#define AVR_SREG 0x5F
#define AVR_SREG_I 7
#define AVR_SPL 0x5D
#define AVR_SPH 0x5E
#define AVR_EIND 0x5C
#define AVR_RAMPZ 0x5B

/* MCUCR: the vector table's place (IVSEL, with the IVCE change enable). */
#define AVR_MCUCR 0x55
#define AVR_IVCE 0
#define AVR_IVSEL 1

/* MCUSR: what caused the last reset. */
#define AVR_MCUSR 0x54
#define AVR_PORF 0
#define AVR_EXTRF 1
#define AVR_BORF 2
#define AVR_WDRF 3
#define AVR_JTRF 4

/* SMCR: the mode the sleep instruction enters; SM = 000 is idle, where the timers and the USART run. */
#define AVR_SMCR 0x53
#define AVR_SE 0
#define AVR_SM0 1
#define AVR_SM1 2
#define AVR_SM2 3

/* CLKPR: the system clock prescaler, written under its four-cycle change enable. */
#define AVR_CLKPR 0x61
#define AVR_CLKPS0 0
#define AVR_CLKPS1 1
#define AVR_CLKPS2 2
#define AVR_CLKPS3 3
#define AVR_CLKPCE 7

/* WDTCSR: the watchdog, written under its four-cycle change enable (WDCE together with WDE). */
#define AVR_WDTCSR 0x60
#define AVR_WDP0 0
#define AVR_WDP1 1
#define AVR_WDP2 2
#define AVR_WDE 3
#define AVR_WDCE 4
#define AVR_WDP3 5
#define AVR_WDIE 6
#define AVR_WDIF 7

/* ---- EEPROM ---- */
#define AVR_EECR 0x3F
#define AVR_EERE 0
#define AVR_EEPE 1
#define AVR_EEMPE 2
#define AVR_EEDR 0x40
#define AVR_EEARL 0x41
#define AVR_EEARH 0x42

/* ---- Timer/Counter0 (8 bits): the millisecond ---- */
#define AVR_TCCR0A 0x44
#define AVR_WGM00 0
#define AVR_WGM01 1
#define AVR_TCCR0B 0x45
#define AVR_CS00 0
#define AVR_CS01 1
#define AVR_CS02 2
#define AVR_TCNT0 0x46
#define AVR_OCR0A 0x47
#define AVR_TIMSK0 0x6E
#define AVR_OCIE0A 1
#define AVR_TIFR0 0x35
#define AVR_OCF0A 1

/* ---- Timer/Counter1 (16 bits): the cycle stopwatch ---- */
#define AVR_TIFR1 0x36
#define AVR_TOV1 0
#define AVR_TIMSK1 0x6F
#define AVR_TOIE1 0
#define AVR_TCCR1A 0x80
#define AVR_TCCR1B 0x81
#define AVR_CS10 0
#define AVR_TCNT1L 0x84
#define AVR_TCNT1H 0x85

/* ---- USART0: the host link ---- */
#define AVR_UCSR0A 0xC0
#define AVR_MPCM0 0
#define AVR_U2X0 1
#define AVR_UPE0 2
#define AVR_DOR0 3
#define AVR_FE0 4
#define AVR_UDRE0 5
#define AVR_TXC0 6
#define AVR_RXC0 7
#define AVR_UCSR0B 0xC1
#define AVR_TXB80 0
#define AVR_RXB80 1
#define AVR_UCSZ02 2
#define AVR_TXEN0 3
#define AVR_RXEN0 4
#define AVR_UDRIE0 5
#define AVR_TXCIE0 6
#define AVR_RXCIE0 7
#define AVR_UCSR0C 0xC2
#define AVR_UCPOL0 0
#define AVR_UCSZ00 1
#define AVR_UCSZ01 2
#define AVR_USBS0 3
#define AVR_UPM00 4
#define AVR_UPM01 5
#define AVR_UMSEL00 6
#define AVR_UMSEL01 7
#define AVR_UBRR0L 0xC4
#define AVR_UBRR0H 0xC5
#define AVR_UDR0 0xC6

/* ---- Power reduction ---- */
#define AVR_PRR0 0x64
#define AVR_PRUSART0 1
#define AVR_PRTIM1 3
#define AVR_PRTIM0 5
#define AVR_PRR1 0x65

/* ---- Port E: the USART0 pads, RXD0 on PE0 and TXD0 on PE1 ---- */
#define AVR_PINE 0x2C
#define AVR_DDRE 0x2D
#define AVR_PORTE 0x2E

/* ---- Port D: the front end's PA_CSD on PD6 (docs/hardware_avr.md §11.1) ---- */
#define AVR_DDRD 0x2A
#define AVR_PORTD 0x2B
#define AVR_PORTD6 6

/* ---- The transceiver: register n of the AT86RF233 at 0x140 + n (docs/hardware_avr.md §11.4) ---- */
#define AVR_PRTRX24 6 /* in PRR1 */
#define AVR_TRXPR 0x139
#define AVR_TRXRST 0
#define AVR_SLPTR 1
#define AVR_AES_CTRL 0x13C
#define AVR_AES_REQUEST 7
#define AVR_AES_STATUS 0x13D
#define AVR_AES_DONE 0
#define AVR_AES_STATE 0x13E
#define AVR_AES_KEY 0x13F
#define AVR_TRX_STATUS 0x141
#define AVR_TRX_STATUS0 0 /* the state, five bits */
#define AVR_CCA_STATUS 6
#define AVR_CCA_DONE 7
#define AVR_TRX_STATE 0x142
#define AVR_TRX_CMD0 0 /* the command, five bits */
#define AVR_TRAC_STATUS0 5 /* the transaction status, three bits */
#define AVR_TRX_CTRL_1 0x144
#define AVR_TX_AUTO_CRC_ON 5
#define AVR_PA_EXT_EN 7
#define AVR_PHY_TX_PWR 0x145
#define AVR_TX_PWR0 0 /* four bits */
#define AVR_PHY_RSSI 0x146
#define AVR_RSSI0 0 /* five bits */
#define AVR_RX_CRC_VALID 7
#define AVR_PHY_ED_LEVEL 0x147
#define AVR_PHY_CC_CCA 0x148
#define AVR_CHANNEL0 0 /* five bits */
#define AVR_CCA_MODE0 5 /* two bits */
#define AVR_CCA_REQUEST 7
#define AVR_CCA_THRES 0x149
#define AVR_CCA_ED_THRES0 0 /* four bits */
#define AVR_TRX_CTRL_2 0x14C
#define AVR_RX_SAFE_MODE 7
#define AVR_ANT_DIV 0x14D
#define AVR_ANT_CTRL0 0 /* two bits */
#define AVR_ANT_EXT_SW_EN 2
#define AVR_ANT_DIV_EN 3
#define AVR_IRQ_MASK 0x14E
#define AVR_RX_END_EN 3
#define AVR_CCA_ED_DONE_EN 4
#define AVR_TX_END_EN 6
#define AVR_IRQ_STATUS 0x14F
#define AVR_RX_END 3
#define AVR_CCA_ED_DONE 4
#define AVR_TX_END 6
#define AVR_PART_NUM 0x15C
#define AVR_SHORT_ADDR_0 0x160
#define AVR_SHORT_ADDR_1 0x161
#define AVR_PAN_ID_0 0x162
#define AVR_PAN_ID_1 0x163
#define AVR_IEEE_ADDR_0 0x164
#define AVR_XAH_CTRL_0 0x16C
#define AVR_MAX_CSMA_RETRIES0 1 /* three bits */
#define AVR_MAX_FRAME_RETRIES0 4 /* four bits */
#define AVR_CSMA_SEED_0 0x16D
#define AVR_TST_RX_LENGTH 0x17B
#define AVR_TRXFBST 0x180
#define AVR_TRXFBEND 0x1FF

/* ---- Interrupt vectors: the slot numbers of the table in startup_avr.S ---- */
#define AVR__VECTORS_SIZE 308
#define AVR_TIMER1_OVF_vect_num 20
#define AVR_TIMER0_COMPA_vect_num 21
#define AVR_USART0_RX_vect_num 25
#define AVR_USART0_UDRE_vect_num 26
#define AVR_USART0_TX_vect_num 27
#define AVR_TRX24_RX_END_vect_num 60
#define AVR_TRX24_CCA_ED_DONE_vect_num 61
#define AVR_TRX24_TX_END_vect_num 63

/* ---- This tree's own ---- */
/* The I/O-space address of a register below 0x60, for in/out and the bit instructions. */
#define AVR_OWN_IO(addr) ((addr) - 0x20)

#ifndef __ASSEMBLER__

#include <stdint.h>

#include "hal.h"

#define AVR_REG8(addr) (*(volatile uint8_t *)(uint16_t)(addr))
#define AVR_BIT(n) ((uint8_t)(1u << (n)))

/* The C name of vector slot n, as avr-gcc's back end recognises it: a function called __vector_<n>
 * with the signal attribute is saved, restored and returned from with reti. The slot number has to
 * reach the paste as a literal, so the vector name macro is expanded first. */
#define AVR_OWN_ISR_NAME(n) __vector_##n
#define AVR_OWN_ISR_DECL(n) void AVR_OWN_ISR_NAME(n)(void) __attribute__((signal, used, externally_visible))
#define AVR_OWN_ISR_DEF(n) void AVR_OWN_ISR_NAME(n)(void)
#define AVR_ISR(vect) AVR_OWN_ISR_DECL(vect); AVR_OWN_ISR_DEF(vect)

/* An interrupt lock that keeps the caller's interrupt state, for glue that can run in an interrupt as
 * well as in the loop. HAL_IrqUnlock sets I unconditionally, and on this core an interrupt that sets I
 * opens itself to nesting; restoring SREG does not. Both halves are plain loads and stores of SREG
 * fenced by HAL_Dsb, the compiler barrier: an interrupt between the read and the write returns with
 * SREG as it found it. */
static inline uint8_t AvrOwn_LockSave(void)
{
    uint8_t sreg = AVR_REG8(AVR_SREG);

    AVR_REG8(AVR_SREG) = (uint8_t)(sreg & (uint8_t)~AVR_BIT(AVR_SREG_I));
    HAL_Dsb();
    return sreg;
}

static inline void AvrOwn_LockRestore(uint8_t sreg)
{
    HAL_Dsb();
    AVR_REG8(AVR_SREG) = sreg;
}

#endif /* __ASSEMBLER__ */

#endif /* AVR_REGS_H */
