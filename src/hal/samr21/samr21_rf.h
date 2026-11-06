/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* samr21_rf.h — AT86RF233 access (SPI register + frame buffer + TRX state).
 *
 * The low-level half of the radio HAL, shared by the clock bring-up
 * (samr21_clock.c - CLKM is the DFLL48M reference) and the radio data path
 * (samr21_radio.c). Register addresses, SPI command bytes, TRX state values and
 * the frame-buffer layout are cited at their definitions in samr21_rf.c. */
#ifndef SAMR21_RF_H
#define SAMR21_RF_H

#include <stdint.h>

#include "radio.h" /* RADIO_PSDU_MAX: the frame buffer holds a 127-byte PSDU */

/* One received frame as the RF233 hands it over: [PHY_STATUS, PHR, PSDU..., LQI,
 * ED, RX_STATUS] = 5 + frame_length bytes (at86rf233.txt:2343-2348). PHY_STATUS is
 * the byte clocked out while the command byte goes in. */
typedef struct
{
    uint8_t psdu[RADIO_PSDU_MAX];
    uint8_t len; /* PHR: PSDU bytes, FCS included */
    uint8_t lqi;
    uint8_t ed;
    uint8_t rxStatus; /* bit 7 = RX_CRC_VALID (Table 6-3) */
    uint8_t phyStatus;
} Rf_RxFrame;

/* Pins + SERCOM4; safe to call again, and does NOT reset the transceiver. 1 = SPI
 * and transceiver answer. */
int Rf_HwInit(void);
int Rf_HwReady(void);

/* Register access. 6-bit address space; 1 = the transaction completed. */
int Rf_RegRead(uint8_t addr, uint8_t *value);
int Rf_RegWrite(uint8_t addr, uint8_t value);

/* Bounded, watchdog-fed wait until TRX_STATUS' state field equals `want`; 0 on the
 * bound. */
int Rf_TrxCmd(uint8_t cmd);
int Rf_TrxStatus(uint8_t *status);
int Rf_WaitTrxState(uint8_t want);
/* RX_AACK_ON reads 0x16 idle and 0x11 (BUSY_RX_AACK) during a receive; which one a
 * caller sees is not predictable (at86rf233.txt Table 7-3). */
int Rf_WaitTrxStateAny(uint8_t wantA, uint8_t wantB);

/* IRQ_STATUS, read and cleared (the RF233 clears a latched IRQ by writing the
 * bit back). Bit 3 = TRX_END, bit 6 = TRX_UR (frame buffer underrun/overwrite),
 * bit 4 = CCA_ED_DONE, bit 2 = RX_START (at86rf233.txt:3284-3320). */
int Rf_IrqStatus(uint8_t *status);

/* Frame buffer. Read: one frame (5 + len layout). Write: a PSDU with its FCS, PHR = len,
 * what TX_AUTO_CRC_ON expects - "for a frame with a frame length specified as N, the FCS is
 * calculated on the first N-2 octets" (at86rf233.txt:10984-10989), the last two octets being
 * the checksum it writes there. */
int Rf_FrameBufferRead(Rf_RxFrame *out);
int Rf_FrameBufferWrite(const uint8_t *psdu, uint8_t len);

/* Bytes of the Frame Buffer directly: its address 0 is the PHR. The transceiver reads the buffer as it
 * transmits, so an octet not yet on air can still be changed (at86rf233 datasheet 9.3, 10.2), and this
 * mode raises no access violation for it. */
int Rf_SramWrite(uint8_t addr, const uint8_t *src, uint8_t n);

/* The AES engine, at the bus's full speed (at86rf233 datasheet 11.1). Usable in every state but sleep, a
 * reception or a transmission included. Rf_AesEncrypt is one ECB block; the key stays set. */
int Rf_AesSetKey(const uint8_t key[16]);
int Rf_AesEncrypt(const uint8_t in[16], uint8_t out[16]);

/* SCLK = gen0 / (2 * (BAUD + 1)). After the CPU switches to the DFLL, gen0 is
 * 48 MHz and the bring-up BAUD would give 12 MHz - above the RF233's 8 MHz limit
 * (at86rf233.txt:20690-20700, fsync max 8 MHz); the radio path sets its own
 * divisor. */
int Rf_SetSpiBaud(uint8_t baud);

/* The external front end: the transceiver's DIG4/DIG3 RX/TX indicator (TRX_CTRL_1.PA_EXT_EN)
 * routed through RFCTRL to PA08/PA09, and the board's enable pins on PORT A driven high
 * (docs/hardware_samr21.md §6). A board with no front end passes 0 and nothing is touched. */
void Rf_FrontEndInit(uint32_t enablePins);

/* The transceiver's IRQ line (PB00, EIC EXTINT0, level-high) and the SPI ownership rule. */
void Rf_IrqLineInit(void);
void Rf_IrqArm(void);
int Rf_IrqArmed(void);
int Rf_IrqPinHigh(void);

/* Thread context: hold the SPI against the radio vector across a multi-step sequence. Nests;
 * a no-op in the handler and before the vector is armed. Every Rf_* call takes it itself. */
void Rf_Lock(void);
void Rf_Unlock(void);

/* Handler context only. */
void Rf_HandlerEnter(void);
void Rf_HandlerExit(void);
int Rf_HandlerIrqStatus(uint8_t *status);

/* [PHR, MPDU...] exactly as given: the Ack's four octets. */
int Rf_FrameBufferWriteRaw(const uint8_t *buf, uint8_t n);

/* PART_NUM (0x1C) = 0x0B for the AT86RF233 (Table 6-5). */
int Rf_PartNum(uint8_t *value);

#endif
